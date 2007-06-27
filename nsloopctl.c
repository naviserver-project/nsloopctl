/*
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://mozilla.org/.
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
 * the License for the specific language governing rights and limitations
 * under the License.
 *
 * The Original Code is AOLserver Code and related documentation
 * distributed by AOL.
 * 
 * The Initial Developer of the Original Code is America Online,
 * Inc. Portions created by AOL are Copyright (C) 1999 America Online,
 * Inc. All Rights Reserved.
 *
 * Alternatively, the contents of this file may be used under the terms
 * of the GNU General Public License (the "GPL"), in which case the
 * provisions of GPL are applicable instead of those above.  If you wish
 * to allow use of your version of this file only under the terms of the
 * GPL and not to allow others to use your version of this file under the
 * License, indicate your decision by deleting the provisions above and
 * replace them with the notice and other provisions required by the GPL.
 * If you do not delete the provisions above, a recipient may use your
 * version of this file under either the License or the GPL.
 */

/* 
 * nsloopctl.c --
 *
 *      Replacements for the "for", "while", and "foreach" commands to be
 *      monitored and managed by "ns_loopctl" command.
 */

#include "ns.h"

NS_RCSID("$Header$");


/*
 * The following structure supports sending a script to a
 * loop to eval.
 */

typedef struct EvalData {
    enum {
        EVAL_WAIT,
        EVAL_DONE,
        EVAL_DROP
    } state;                /* Eval request state. */

    int         code;       /* Script result code. */
    Tcl_DString script;     /* Script buffer. */
    Tcl_DString result;     /* Result buffer. */

} EvalData;

/*
 * The following structure is allocated for the "while"
 * and "for" commands to maintain a copy of the current
 * args and provide a cancel flag.
 */

typedef struct LoopData {
    enum {
        LOOP_RUN,
        LOOP_PAUSE,
        LOOP_CANCEL
    } control;              /* Loop control commands. */

    char           lid[32]; /* Unique loop id. */
    intptr_t       tid;     /* Thread id of script. */
    unsigned int   spins;   /* Loop iterations. */
    Ns_Time        etime;   /* Loop entry time. */
    Tcl_HashEntry *hPtr;    /* Entry in active loop table. */
    Tcl_DString    args;    /* Copy of command args. */
    EvalData      *evalPtr; /* Eval request pending. */

} LoopData;

/*
 * The following structure maintains per-thread context to support
 * a shared async cancel object.
 */

typedef struct ThreadData {
    Tcl_AsyncHandler  cancel;
    Tcl_HashEntry    *hPtr;    /* Self reference to threads table. */
} ThreadData;


/*
 * Static procedures defined in this file.
 */

static Tcl_ObjCmdProc  ForObjCmd, WhileObjCmd, ForeachObjCmd, LoopCtlObjCmd;
static Ns_TclTraceProc InitInterp;

static int CheckControl(Tcl_Interp *interp, LoopData *loopPtr);
static void EnterLoop(LoopData *loopPtr, int objc, Tcl_Obj * CONST objv[]);
static void LeaveLoop(LoopData *loopPtr);

static Ns_TlsCleanup ThreadCleanup;
static Tcl_AsyncProc ThreadAbort;


/*
 * Static variables defined in this file.
 */

static Tcl_HashTable loops;   /* Currently running loops. */
static Tcl_HashTable threads; /* Running threads with interps allocated. */
static Ns_Tls        tls;     /* Slot for per-thread cancel cookie. */
static Ns_Mutex      lock;    /* Lock around loops and threads tables. */
static Ns_Cond       cond;    /* Wait for evaluation to complete. */



/*
 *----------------------------------------------------------------------
 *
 * Ns_ModuleInit --
 *
 *      Initialise the nsloop variables and enable the loop commands
 *      for this virtual server.
 *
 * Results:
 *      NS_OK or NS_ERROR.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ModuleInit(CONST char *server, CONST char *module)
{
    static int once = 0;

    Ns_MutexLock(&lock);
    if (!once) {
        once = 1;
        Ns_MutexSetName(&lock, "nsloopctl");
        Tcl_InitHashTable(&loops, TCL_STRING_KEYS);
        Tcl_InitHashTable(&threads, TCL_STRING_KEYS);
        Ns_TlsAlloc(&tls, ThreadCleanup);
    }
    Ns_MutexUnlock(&lock);

    if (server == NULL) {
        Ns_Log(Error, "nsloopctl: module must be loaded into a virtual server.");
        return NS_ERROR;
    }

    Ns_TclRegisterTrace(server, InitInterp, NULL, NS_TCL_TRACE_CREATE);
    Ns_RegisterProcInfo(InitInterp, "nsloopctl:initinterp", NULL);

    return NS_OK;
}

static int
InitInterp(Tcl_Interp *interp, void *arg)
{
    ThreadData  *threadPtr;
    char         tid[32];
    int          new;

    /*
     * Make sure the thread in which this interp is running has
     * been initialised for async signals.
     */

    threadPtr = Ns_TlsGet(&tls);
    if (threadPtr == NULL) {
        threadPtr = ns_malloc(sizeof(ThreadData));
        threadPtr->cancel = Tcl_AsyncCreate(ThreadAbort, NULL);
        snprintf(tid, sizeof(tid), "%" PRIxPTR, Ns_ThreadId());
        Ns_MutexLock(&lock);
        threadPtr->hPtr = Tcl_CreateHashEntry(&threads, tid, &new);
        Tcl_SetHashValue(threadPtr->hPtr, threadPtr);
        Ns_MutexUnlock(&lock);
        Ns_TlsSet(&tls, threadPtr);
    }

    Tcl_CreateObjCommand(interp, "ns_loopctl", LoopCtlObjCmd, threadPtr, NULL);
    Tcl_CreateObjCommand(interp, "for",        ForObjCmd,     NULL, NULL);
    Tcl_CreateObjCommand(interp, "while",      WhileObjCmd,   NULL, NULL);
    Tcl_CreateObjCommand(interp, "foreach",    ForeachObjCmd, NULL, NULL);

    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * LoopCtlObjCmd --
 *
 *      Control command to list all active for or while loops in
 *      any thread, get info (thread id and args) for an active
 *      loop, or signal cancel of a loop.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      May cancel an active loop.  Not cancel results in a
 *      TCL_ERROR result for the "for" or "while" command,
 *      an exception which can possibly be caught.
 *
 *----------------------------------------------------------------------
 */

static int
LoopCtlObjCmd(arg, interp, objc, objv)
     ClientData arg;                     /* Pointer to NsInterp. */
     Tcl_Interp *interp;                 /* Current interpreter. */
     int objc;                           /* Number of arguments. */
     Tcl_Obj *CONST objv[];              /* Argument objects. */
{
    LoopData       *loopPtr;
    EvalData        eval;
    ThreadData     *threadPtr;
    Tcl_HashTable  *tablePtr;
    Tcl_HashEntry  *hPtr;
    Tcl_HashSearch  search;
    Ns_Time         timeout;
    char           *id;
    char           *str;
    Tcl_Obj        *objPtr, *listPtr;
    int             result, len, status;

    static CONST char *opts[] = {
        "abort", "cancel", "eval", "info", "loops",
        "pause", "resume", "threads", NULL
    };
    enum {
        LAbortIdx, LCancelIdx, LEvalIdx,  LInfoIdx, LLoopsIdx,
        LPauseIdx, LResumeIdx, LThreadsIdx
    } opt;

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "option ?id?");
        return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[1], opts, "option", 0,
                            (int *) &opt) != TCL_OK) {
        return TCL_ERROR;
    }

    /*
     * Handle the loops and threads commands and verify
     * arguments first.
     */

    switch (opt) {

    case LLoopsIdx:
    case LThreadsIdx:
        if (objc != 2) {
            Tcl_WrongNumArgs(interp, 2, objv, NULL);
            return TCL_ERROR;
        }
        tablePtr = (opt == LLoopsIdx) ? &loops : &threads;
        listPtr = Tcl_NewListObj(0, NULL);

        Ns_MutexLock(&lock);
        hPtr = Tcl_FirstHashEntry(tablePtr, &search);
        while (hPtr != NULL) {
            objPtr = Tcl_NewStringObj(Tcl_GetHashKey(tablePtr, hPtr), -1);
            Tcl_ListObjAppendElement(interp, listPtr, objPtr);
            hPtr = Tcl_NextHashEntry(&search);
        }
        Ns_MutexUnlock(&lock);

        Tcl_SetObjResult(interp, listPtr);
        return TCL_OK;

        break;

    case LAbortIdx:
        if (objc != 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "id");
            return TCL_ERROR;
        }
        id = Tcl_GetString(objv[2]);

        Ns_MutexLock(&lock);
        hPtr = Tcl_FindHashEntry(&threads, id);
        if (hPtr != NULL) {
            threadPtr = Tcl_GetHashValue(hPtr);
            Tcl_AsyncMark(threadPtr->cancel);
            Tcl_SetObjResult(interp, Tcl_NewBooleanObj(1));
            result = TCL_OK;
        } else {
            Tcl_AppendResult(interp, "no such active thread: ", id, NULL);
            result = TCL_ERROR;
        }
        Ns_MutexUnlock(&lock);

        return result;
        break;

    case LEvalIdx:
        if (objc != 4) {
            Tcl_WrongNumArgs(interp, 2, objv, "id script");
            return TCL_ERROR;
        }
        break;

    default:
        if (objc != 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "id");
            return TCL_ERROR;
        }
        break;
    }

    /*
     * All other commands require an opaque loop id arg.
     */

    id = Tcl_GetString(objv[2]);
    result = TCL_OK;

    Ns_MutexLock(&lock);
    hPtr = Tcl_FindHashEntry(&loops, id);
    if (hPtr == NULL) {
        switch (opt) {
        case LInfoIdx:
        case LEvalIdx:
            Tcl_AppendResult(interp, "no such loop id: ",
                             Tcl_GetString(objv[2]), NULL);
            result = TCL_ERROR;
            break;
        default:
            Tcl_SetObjResult(interp, Tcl_NewBooleanObj(0));
            break;
        }
        goto done;
    }

    loopPtr = Tcl_GetHashValue(hPtr);

    switch (opt) {
    case LLoopsIdx:
    case LThreadsIdx:
    case LAbortIdx:
        /* NB: Silence warning. */
        break;

    case LInfoIdx:

        switch (loopPtr->control) {
        case LOOP_RUN:
            str = "running";
            break;
        case LOOP_PAUSE:
            str = "paused";
            break;
        case LOOP_CANCEL:
            str = "canceled";
            break;
        default:
            str = "";
            break;
        }
        Ns_TclPrintfResult(interp,
            "loopid %s threadid %" PRIxPTR " start %jd:%ld "
            "spins %u status %s command {%s}",
            id, loopPtr->tid, (intmax_t) loopPtr->etime.sec, loopPtr->etime.usec,
            loopPtr->spins, str, loopPtr->args.string);
        break;

    case LEvalIdx:
        if (loopPtr->evalPtr != NULL) {
            Tcl_SetResult(interp, "eval pending", TCL_STATIC);
            result = TCL_ERROR;
            goto done;
        }

        /*
         * Queue new script to eval.
         */

        eval.state = EVAL_WAIT;
        eval.code = TCL_OK;
        Tcl_DStringInit(&eval.result);
        Tcl_DStringInit(&eval.script);
        str = Tcl_GetStringFromObj(objv[3], &len);
        Tcl_DStringAppend(&eval.script, str, len);
        loopPtr->evalPtr = &eval;

        /*
         * Wait for result.
         */

        Ns_GetTime(&timeout);
        Ns_IncrTime(&timeout, 2, 0);
        Ns_CondBroadcast(&cond);

        status = NS_OK;
        while (status == NS_OK && eval.state == EVAL_WAIT) {
            status = Ns_CondTimedWait(&cond, &lock, &timeout);
        }

        switch (eval.state) {
        case EVAL_WAIT:
            Tcl_SetResult(interp, "timeout: result dropped", TCL_STATIC);
            loopPtr->evalPtr = NULL;
            result = TCL_ERROR;
            break;
        case EVAL_DROP:
            Tcl_SetResult(interp, "dropped: loop exited", TCL_STATIC);
            result = TCL_ERROR;
            break;
        case EVAL_DONE:
            Tcl_DStringResult(interp, &eval.result);
            result = eval.code;
        }
        Tcl_DStringFree(&eval.script);
        Tcl_DStringFree(&eval.result);
        break;

    case LResumeIdx:
    case LPauseIdx:
    case LCancelIdx:
        if (opt == LCancelIdx) {
            loopPtr->control = LOOP_CANCEL;
        } else if (opt == LPauseIdx) {
            loopPtr->control = LOOP_PAUSE;
        } else {
            loopPtr->control = LOOP_RUN;
        }
        Tcl_SetObjResult(interp, Tcl_NewBooleanObj(1));
        Ns_CondBroadcast(&cond);
        break;
    }

 done:
    Ns_MutexUnlock(&lock);

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * ForObjCmd --
 *
 *      This procedure is invoked to process the "for" Tcl command.
 *      See the user documentation for details on what it does.
 *
 *  With the bytecode compiler, this procedure is only called when
 *  a command name is computed at runtime, and is "for" or the name
 *  to which "for" was renamed: e.g.,
 *  "set z for; $z {set i 0} {$i<100} {incr i} {puts $i}"
 *
 *  Copied from the Tcl source with additional calls to the 
 *  loop control facility.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      See the user documentation.
 *
 *----------------------------------------------------------------------
 */

static int
ForObjCmd(arg, interp, objc, objv)
     ClientData arg;                     /* Pointer to NsInterp. */
     Tcl_Interp *interp;                 /* Current interpreter. */
     int objc;                           /* Number of arguments. */
     Tcl_Obj *CONST objv[];              /* Argument objects. */
{
    LoopData  data;
    int       result, value;

    if (objc != 5) {
        Tcl_WrongNumArgs(interp, 1, objv, "start test next command");
        return TCL_ERROR;
    }

    result = Tcl_EvalObjEx(interp, objv[1], 0);
    if (result != TCL_OK) {
        if (result == TCL_ERROR) {
            Tcl_AddErrorInfo(interp, "\n    (\"for\" initial command)");
        }
        return result;
    }

    EnterLoop(&data, objc, objv);

    while (1) {
        /*
         * We need to reset the result before passing it off to
         * Tcl_ExprBooleanObj.  Otherwise, any error message will be appended
         * to the result of the last evaluation.
         */

        Tcl_ResetResult(interp);
        result = Tcl_ExprBooleanObj(interp, objv[2], &value);
        if (result != TCL_OK) {
            goto done;
        }
        if (!value) {
            break;
        }
        result = CheckControl(interp, &data);
        if (result == TCL_OK) {
            result = Tcl_EvalObjEx(interp, objv[4], 0);
        }
        if ((result != TCL_OK) && (result != TCL_CONTINUE)) {
            if (result == TCL_ERROR) {
                char msg[32 + TCL_INTEGER_SPACE];

                sprintf(msg, "\n    (\"for\" body line %d)",interp->errorLine);
                Tcl_AddErrorInfo(interp, msg);
            }
            break;
        }

        result = Tcl_EvalObjEx(interp, objv[3], 0);

        if (result == TCL_BREAK) {
            break;
        } else if (result != TCL_OK) {
            if (result == TCL_ERROR) {
                Tcl_AddErrorInfo(interp, "\n    (\"for\" loop-end command)");
            }
            goto done;
        }
    }
    if (result == TCL_BREAK) {
        result = TCL_OK;
    }
    if (result == TCL_OK) {
        Tcl_ResetResult(interp);
    }
 done:
    LeaveLoop(&data);

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * WhileObjCmd --
 *
 *      This procedure is invoked to process the "while" Tcl command.
 *      See the user documentation for details on what it does.
 *
 *  With the bytecode compiler, this procedure is only called when
 *  a command name is computed at runtime, and is "while" or the name
 *  to which "while" was renamed: e.g., "set z while; $z {$i<100} {}"
 *
 *  Copied from the Tcl source with additional calls to the 
 *  loop control facility.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      See the user documentation.
 *
 *----------------------------------------------------------------------
 */

static int
WhileObjCmd(arg, interp, objc, objv)
     ClientData arg;                     /* Pointer to NsInterp. */
     Tcl_Interp *interp;                 /* Current interpreter. */
     int objc;                           /* Number of arguments. */
     Tcl_Obj *CONST objv[];              /* Argument objects. */
{
    LoopData  data;
    int       result, value;

    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "test command");
        return TCL_ERROR;
    }

    EnterLoop(&data, objc, objv);

    while (1) {
        result = Tcl_ExprBooleanObj(interp, objv[1], &value);
        if (result != TCL_OK) {
            goto done;
        }
        if (!value) {
            break;
        }
        result = CheckControl(interp, &data);
        if (result == TCL_OK) {
            result = Tcl_EvalObjEx(interp, objv[2], 0);
        }
        if ((result != TCL_OK) && (result != TCL_CONTINUE)) {
            if (result == TCL_ERROR) {
                char msg[32 + TCL_INTEGER_SPACE];

                sprintf(msg, "\n    (\"while\" body line %d)",
                        interp->errorLine);
                Tcl_AddErrorInfo(interp, msg);
            }
            break;
        }
    }
    if (result == TCL_BREAK) {
        result = TCL_OK;
    }
    if (result == TCL_OK) {
        Tcl_ResetResult(interp);
    }
 done:
    LeaveLoop(&data);

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * ForeachObjCmd --
 *
 *      This object-based procedure is invoked to process the "foreach" Tcl
 *      command.  See the user documentation for details on what it does.
 *
 * Results:
 *      A standard Tcl object result.
 *
 * Side effects:
 *      See the user documentation.
 *
 *----------------------------------------------------------------------
 */

static int
ForeachObjCmd(arg, interp, objc, objv)
     ClientData arg;             /* Pointer to NsInterp. */
     Tcl_Interp *interp;         /* Current interpreter. */
     int objc;                   /* Number of arguments. */
     Tcl_Obj *CONST objv[];      /* Argument objects. */
{
    LoopData  data;
    int       result = TCL_OK;
    int       i;              /* i selects a value list */
    int       j, maxj;        /* Number of loop iterations */
    int       v;              /* v selects a loop variable */
    int       numLists;       /* Count of value lists */
    Tcl_Obj  *bodyPtr;

    /*
     * We copy the argument object pointers into a local array to avoid
     * the problem that "objv" might become invalid. It is a pointer into
     * the evaluation stack and that stack might be grown and reallocated
     * if the loop body requires a large amount of stack space.
     */
    
#define NUM_ARGS 9
    Tcl_Obj *(argObjStorage[NUM_ARGS]);
    Tcl_Obj **argObjv = argObjStorage;
    
#define STATIC_LIST_SIZE 4
    int indexArray[STATIC_LIST_SIZE];
    int varcListArray[STATIC_LIST_SIZE];
    Tcl_Obj **varvListArray[STATIC_LIST_SIZE];
    int argcListArray[STATIC_LIST_SIZE];
    Tcl_Obj **argvListArray[STATIC_LIST_SIZE];

    int *index = indexArray;               /* Array of value list indices */
    int *varcList = varcListArray;         /* # loop variables per list */
    Tcl_Obj ***varvList = varvListArray;   /* Array of var name lists */
    int *argcList = argcListArray;         /* Array of value list sizes */
    Tcl_Obj ***argvList = argvListArray;   /* Array of value lists */

    if (objc < 4 || (objc%2 != 0)) {
        Tcl_WrongNumArgs(interp, 1, objv,
                         "varList list ?varList list ...? command");
        return TCL_ERROR;
    }

    EnterLoop(&data, objc, objv);

    /*
     * Create the object argument array "argObjv". Make sure argObjv is
     * large enough to hold the objc arguments.
     */

    if (objc > NUM_ARGS) {
        argObjv = (Tcl_Obj **) ckalloc(objc * sizeof(Tcl_Obj *));
    }
    for (i = 0;  i < objc;  i++) {
        argObjv[i] = objv[i];
    }

    /*
     * Manage numList parallel value lists.
     * argvList[i] is a value list counted by argcList[i]
     * varvList[i] is the list of variables associated with the value list
     * varcList[i] is the number of variables associated with the value list
     * index[i] is the current pointer into the value list argvList[i]
     */

    numLists = (objc-2)/2;
    if (numLists > STATIC_LIST_SIZE) {
        index = (int *) ckalloc(numLists * sizeof(int));
        varcList = (int *) ckalloc(numLists * sizeof(int));
        varvList = (Tcl_Obj ***) ckalloc(numLists * sizeof(Tcl_Obj **));
        argcList = (int *) ckalloc(numLists * sizeof(int));
        argvList = (Tcl_Obj ***) ckalloc(numLists * sizeof(Tcl_Obj **));
    }
    for (i = 0;  i < numLists;  i++) {
        index[i] = 0;
        varcList[i] = 0;
        varvList[i] = (Tcl_Obj **) NULL;
        argcList[i] = 0;
        argvList[i] = (Tcl_Obj **) NULL;
    }

    /*
     * Break up the value lists and variable lists into elements
     */

    maxj = 0;
    for (i = 0;  i < numLists;  i++) {
        result = Tcl_ListObjGetElements(interp, argObjv[1+i*2],
                                        &varcList[i], &varvList[i]);
        if (result != TCL_OK) {
            goto done;
        }
        if (varcList[i] < 1) {
            Tcl_AppendToObj(Tcl_GetObjResult(interp),
                            "foreach varlist is empty", -1);
            result = TCL_ERROR;
            goto done;
        }

        result = Tcl_ListObjGetElements(interp, argObjv[2+i*2],
                                        &argcList[i], &argvList[i]);
        if (result != TCL_OK) {
            goto done;
        }

        j = argcList[i] / varcList[i];
        if ((argcList[i] % varcList[i]) != 0) {
            j++;
        }
        if (j > maxj) {
            maxj = j;
        }
    }

    /*
     * Iterate maxj times through the lists in parallel
     * If some value lists run out of values, set loop vars to ""
     */

    bodyPtr = argObjv[objc-1];
    for (j = 0;  j < maxj;  j++) {
        for (i = 0;  i < numLists;  i++) {
            /*
             * Refetch the list members; we assume that the sizes are
             * the same, but the array of elements might be different
             * if the internal rep of the objects has been lost and
             * recreated (it is too difficult to accurately tell when
             * this happens, which can lead to some wierd crashes,
             * like Bug #494348...)
             */

            result = Tcl_ListObjGetElements(interp, argObjv[1+i*2],
                                            &varcList[i], &varvList[i]);
            if (result != TCL_OK) {
                panic("nsloop: ForeachObjCmd: could not reconvert variable list %d to a list object\n", i);
            }
            result = Tcl_ListObjGetElements(interp, argObjv[2+i*2],
                                            &argcList[i], &argvList[i]);
            if (result != TCL_OK) {
                panic("nsloop: ForeachObjCmd: could not reconvert value list %d to a list object\n", i);
            }

            for (v = 0;  v < varcList[i];  v++) {
                int k = index[i]++;
                Tcl_Obj *valuePtr, *varValuePtr;

                if (k < argcList[i]) {
                    valuePtr = argvList[i][k];
                } else {
                    valuePtr = Tcl_NewObj(); /* empty string */
                }
                Tcl_IncrRefCount(valuePtr);
                varValuePtr = Tcl_ObjSetVar2(interp, varvList[i][v],
                                             NULL, valuePtr, 0);
                Tcl_DecrRefCount(valuePtr);
                if (varValuePtr == NULL) {
                    Tcl_ResetResult(interp);
                    Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
                                           "couldn't set loop variable: \"",
                                           Tcl_GetString(varvList[i][v]), "\"", (char *) NULL);
                    result = TCL_ERROR;
                    goto done;
                }

            }
        }

        result = CheckControl(interp, &data);

        if (result == TCL_OK) {
            result = Tcl_EvalObjEx(interp, bodyPtr, 0);
        }
        if (result != TCL_OK) {
            if (result == TCL_CONTINUE) {
                result = TCL_OK;
            } else if (result == TCL_BREAK) {
                result = TCL_OK;
                break;
            } else if (result == TCL_ERROR) {
                char msg[32 + TCL_INTEGER_SPACE];

                sprintf(msg, "\n    (\"foreach\" body line %d)",
                        interp->errorLine);
                Tcl_AddObjErrorInfo(interp, msg, -1);
                break;
            } else {
                break;
            }
        }
    }
    if (result == TCL_OK) {
        Tcl_ResetResult(interp);
    }

 done:
    if (numLists > STATIC_LIST_SIZE) {
        ckfree((char *) index);
        ckfree((char *) varcList);
        ckfree((char *) argcList);
        ckfree((char *) varvList);
        ckfree((char *) argvList);
    }
    if (argObjv != argObjStorage) {
        ckfree((char *) argObjv);
    }

    LeaveLoop(&data);

    return result;

#undef STATIC_LIST_SIZE
#undef NUM_ARGS
}


/*
 *----------------------------------------------------------------------
 *
 * EnterLoop --
 *
 *      Add entry for the LoopData structure when a "for" or
 *      "while" command starts.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Loop can be monitored and possibly canceled by "loop.ctl".
 *
 *----------------------------------------------------------------------
 */

static void
EnterLoop(LoopData *loopPtr, int objc, Tcl_Obj * CONST objv[])
{
    int i, new;
    static unsigned int next = 0;

    loopPtr->control = LOOP_RUN;
    loopPtr->spins = 0;
    loopPtr->tid = Ns_ThreadId();
    loopPtr->evalPtr = NULL;
    Ns_GetTime(&loopPtr->etime);

    /* NB: Must copy strings in case loop body updates or invalidates them. */

    Tcl_DStringInit(&loopPtr->args);
    for (i = 0; i < objc; ++i) {
        Tcl_DStringAppendElement(&loopPtr->args, Tcl_GetString(objv[i]));
    }

    Ns_MutexLock(&lock);
    do {
        snprintf(loopPtr->lid, sizeof(loopPtr->lid),
                 "%" PRIxPTR, (intptr_t) next++);
        loopPtr->hPtr = Tcl_CreateHashEntry(&loops, loopPtr->lid, &new);
    } while (!new);

    Tcl_SetHashValue(loopPtr->hPtr, loopPtr); 
    Ns_MutexUnlock(&lock);
}


/*
 *----------------------------------------------------------------------
 *
 * LeaveLoop --
 *
 *      Remove entry for the LoopData structure when a "for" or
 *      "while" command exits.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
LeaveLoop(LoopData *loopPtr)
{
    Ns_MutexLock(&lock);
    if (loopPtr->evalPtr != NULL) {
        loopPtr->evalPtr->state = EVAL_DROP;
        Ns_CondBroadcast(&cond);
    }
    Tcl_DeleteHashEntry(loopPtr->hPtr);
    Ns_MutexUnlock(&lock);
    Tcl_DStringFree(&loopPtr->args);
}


/*
 *----------------------------------------------------------------------
 *
 * CheckControl --
 *
 *      Check for control flag within a loop of a cancel or pause.
 *
 * Results:
 *      TCL_OK if not canceled, TCL_ERROR otherwise.
 *
 * Side effects:
 *      Leave cancel message as interp result.
 *
 *----------------------------------------------------------------------
 */

static int
CheckControl(Tcl_Interp *interp, LoopData *loopPtr)
{
    Tcl_DString  script;
    char        *str;
    int          result, len;

    Ns_MutexLock(&lock);
    ++loopPtr->spins;
    while (loopPtr->evalPtr != NULL || loopPtr->control == LOOP_PAUSE) {
        if (loopPtr->evalPtr != NULL) {
            Tcl_DStringInit(&script);
            Tcl_DStringAppend(&script, loopPtr->evalPtr->script.string,
                              loopPtr->evalPtr->script.length);
            Ns_MutexUnlock(&lock);
            result = Tcl_EvalEx(interp, script.string, script.length, 0);
            Tcl_DStringFree(&script);
            if (result != TCL_OK) {
                Ns_TclLogError(interp);
            }
            Ns_MutexLock(&lock);
            if (loopPtr->evalPtr == NULL) {
                Ns_Log(Error, "nsloopctl: dropped result: %s", interp->result);
            } else {
                str = Tcl_GetStringFromObj(Tcl_GetObjResult(interp), &len);
                Tcl_DStringAppend(&loopPtr->evalPtr->result, str, len);
                loopPtr->evalPtr->state = EVAL_DONE;
                loopPtr->evalPtr = NULL;
                Ns_CondBroadcast(&cond);
            }
        }
        if (loopPtr->control == LOOP_PAUSE) {
            Ns_CondWait(&cond, &lock);
        }
    }
    if (loopPtr->control == LOOP_CANCEL) {
        Tcl_SetResult(interp, "nsloopctl: loop canceled: returning TCL_ERROR",
                      TCL_STATIC);
        result = TCL_ERROR;
    } else {
        result = TCL_OK;
    }
    Ns_MutexUnlock(&lock);

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * ThreadAbort --
 *
 *      Callback which aborts Tcl execution in which ever interp is
 *      currently running in the given thread.
 *
 * Results:
 *      Always TCL_ERROR.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadAbort(ClientData ignored, Tcl_Interp *interp, int code)
{
    if (interp != NULL) {
        Tcl_ResetResult(interp);
        Tcl_SetResult(interp, "nsloopctl: async thread abort: returning TCL_ERROR",
                      TCL_STATIC);
    } else {
        Ns_Log(Warning, "nsloopctl: no interp active");
    }
    Ns_Log(Warning, "nsloopctl: abort");

    return TCL_ERROR;
}


/*
 *----------------------------------------------------------------------
 *
 * ThreadCleanup --
 *
 *      Delete per-thread context at thread exit time.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
ThreadCleanup(void *arg)
{
    ThreadData *threadPtr = arg;

    Ns_MutexLock(&lock);
    Tcl_DeleteHashEntry(threadPtr->hPtr);
    Ns_MutexUnlock(&lock);

    Tcl_AsyncDelete(threadPtr->cancel);
    ns_free(threadPtr);
}
