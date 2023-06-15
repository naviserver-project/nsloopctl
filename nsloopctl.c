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
 *      monitored and managed by "loopctl_*" commands. Monitor threads
 *      with interps and send Tcl async cancel messages.
 */

#include "ns.h"

NS_EXPORT int Ns_ModuleVersion = 1;
NS_EXPORT Ns_ModuleInitProc Ns_ModuleInit;
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
typedef enum {
    LOOP_RUN,
    LOOP_PAUSE,
    LOOP_CANCEL
} LoopControl;

typedef struct LoopData {
    LoopControl    control;              /* Loop control commands. */

    char           lid[32]; /* Unique loop id. */
    uintptr_t      tid;     /* Thread id of script. */
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

static Tcl_ObjCmdProc
    LoopsObjCmd,
    InfoObjCmd,
    EvalObjCmd,
    PauseObjCmd,
    RunObjCmd,
    CancelObjCmd,
    ThreadsObjCmd,
    AbortObjCmd;

static Tcl_ObjCmdProc
    ForObjCmd,
    WhileObjCmd,
    ForeachObjCmd;

static Ns_TclTraceProc InitInterp;
static Ns_TlsCleanup   ThreadCleanup;
static Tcl_AsyncProc   ThreadAbort;

static int CheckControl(Tcl_Interp *interp, LoopData *loopPtr);
static void EnterLoop(LoopData *loopPtr, int objc, Tcl_Obj * const objv[]);
static void LeaveLoop(LoopData *loopPtr);

static int List(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[],
                Tcl_HashTable *tablePtr);
static int Signal(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[],
                  LoopControl signal);
static LoopData *GetLoop(Tcl_Interp *interp, Tcl_Obj *objPtr);


/*
 * Static variables defined in this file.
 */

static Tcl_HashTable loops;        /* Currently running loops. */
static Tcl_HashTable threads;      /* Running threads with interps allocated. */
static Ns_Tls        tls;          /* Slot for per-thread cancel cookie. */
static Ns_Mutex      lock = NULL;  /* Lock around loops and threads tables. */
static Ns_Cond       cond = NULL;  /* Wait for evaluation to complete. */



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

Ns_ReturnCode
Ns_ModuleInit(const char *server, const char *UNUSED(module))
{
    static bool initialized = NS_FALSE;

    Ns_MasterLock();
    if (!initialized) {
        initialized = NS_TRUE;
        Ns_MutexSetName(&lock, "nsloopctl");
        Tcl_InitHashTable(&loops, TCL_STRING_KEYS);
        Tcl_InitHashTable(&threads, TCL_STRING_KEYS);
        Ns_TlsAlloc(&tls, ThreadCleanup);
    }
    Ns_MasterUnlock();

    if (server == NULL) {
        Ns_Log(Error, "nsloopctl: module must be loaded into a virtual server.");
        return NS_ERROR;
    }

    Ns_TclRegisterTrace(server, InitInterp, NULL, NS_TCL_TRACE_CREATE);
    Ns_RegisterProcInfo((ns_funcptr_t)InitInterp, "nsloopctl:initinterp", NULL);

    return NS_OK;
}

static Ns_ReturnCode
InitInterp(Tcl_Interp *interp, const void *UNUSED(arg))
{
    ThreadData  *threadPtr;
    char         tid[32];
    int          new;
    size_t       i;

    static struct {
        const char         *name;
        Tcl_ObjCmdProc     *proc;
    } ctlCmds[] = {
        {"loopctl_loops",   LoopsObjCmd},
        {"loopctl_info",    InfoObjCmd},
        {"loopctl_eval",    EvalObjCmd},
        {"loopctl_pause",   PauseObjCmd},
        {"loopctl_run",     RunObjCmd},
        {"loopctl_cancel",  CancelObjCmd},

        {"loopctl_threads", ThreadsObjCmd},
        {"loopctl_abort",   AbortObjCmd},

        {"for",             ForObjCmd},
        {"while",           WhileObjCmd},
        {"foreach",         ForeachObjCmd}
    };

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


    for (i = 0u; i < sizeof(ctlCmds) / sizeof(ctlCmds[0]); i++) {
        Tcl_CreateObjCommand(interp, ctlCmds[i].name, ctlCmds[i].proc, NULL, NULL);
    }

    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * LoopsObjCmd, ThreadsObjCmd --
 *
 *      Implements loopctl_loops and loopctl_threads: return a list of
 *      running loops / threads.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
LoopsObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    return List(clientData, interp, objc, objv, &loops);
}

static int
ThreadsObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    return List(clientData, interp, objc, objv, &threads);
}

static int
List(ClientData UNUSED(clientData), Tcl_Interp *interp, int UNUSED(objc), Tcl_Obj *const UNUSED(objv[]),
     Tcl_HashTable *tablePtr)
{
    Tcl_Obj        *listPtr, *objPtr;
    Tcl_HashSearch  search;
    Tcl_HashEntry  *hPtr;

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
}


/*
 *----------------------------------------------------------------------
 *
 * InfoObjCmd --
 *
 *      Implements loopctl_info: return state about a running loop.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
InfoObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    LoopData   *loopPtr;
    const char *desc;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "loop-id");
        return TCL_ERROR;
    }

    Ns_MutexLock(&lock);
    if ((loopPtr = GetLoop(interp, objv[1])) == NULL) {
        Ns_MutexUnlock(&lock);
        return TCL_ERROR;
    }

    switch (loopPtr->control) {
    case LOOP_RUN:
        desc = "running";
        break;
    case LOOP_PAUSE:
        desc = "paused";
        break;
    case LOOP_CANCEL:
        desc = "canceled";
        break;
    default:
        desc = "";
        break;
    }
    Ns_TclPrintfResult(interp,
        "loopid %s threadid %" PRIxPTR
        " start %" PRIu64 ":%ld "
        "spins %u status %s command {%s}",
        Tcl_GetString(objv[1]), loopPtr->tid,
        (int64_t) loopPtr->etime.sec, loopPtr->etime.usec,
        loopPtr->spins, desc, loopPtr->args.string);

    Ns_MutexUnlock(&lock);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * EvalObjCmd --
 *
 *      Implements loopctl_eval: evaluate the given script in the context
 *      of a running loop.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
EvalObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    LoopData *loopPtr;
    EvalData  eval;
    char     *script;
    Ns_Time   timeout;
    int       len, result, status;

    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "loop-id script");
        return TCL_ERROR;
    }

    result = TCL_ERROR;

    Ns_MutexLock(&lock);
    if ((loopPtr = GetLoop(interp, objv[1])) == NULL) {
        goto done;
    }

    if (loopPtr->evalPtr != NULL) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("eval pending", -1));
        goto done;
    }

    /*
     * Queue new script to eval.
     */

    eval.state = EVAL_WAIT;
    eval.code = TCL_OK;
    Tcl_DStringInit(&eval.result);
    Tcl_DStringInit(&eval.script);
    script = Tcl_GetStringFromObj(objv[2], &len);
    Tcl_DStringAppend(&eval.script, script, len);
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
        Tcl_SetObjResult(interp, Tcl_NewStringObj("timeout: result dropped", -1));
        loopPtr->evalPtr = NULL;
        break;

    case EVAL_DROP:
        Tcl_SetObjResult(interp, Tcl_NewStringObj("dropped: loop exited", -1));
        break;

    case EVAL_DONE:
        Tcl_DStringResult(interp, &eval.result);
        result = eval.code;
    }
    Tcl_DStringFree(&eval.script);
    Tcl_DStringFree(&eval.result);

 done:
    Ns_MutexUnlock(&lock);

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * PauseObjCmd, RunObjCmd, CancelObjCmd --
 *
 *      Implements loopctl_pause, loopctl_run and loopctl_cancel: send
 *      control signal to a loop.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
PauseObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    return Signal(clientData, interp, objc, objv, LOOP_PAUSE);
}

static int
RunObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    return Signal(clientData, interp, objc, objv, LOOP_RUN);
}

static int
CancelObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    return Signal(clientData, interp, objc, objv, LOOP_CANCEL);
}

static int
Signal(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const objv[],
       LoopControl signal)
{
    LoopData *loopPtr;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "loop-id");
        return TCL_ERROR;
    }

    Ns_MutexLock(&lock);
    if ((loopPtr = GetLoop(interp, objv[1])) == NULL) {
        Ns_MutexUnlock(&lock);
        return TCL_ERROR;
    }

    loopPtr->control = signal;
    Ns_CondBroadcast(&cond);

    Ns_MutexUnlock(&lock);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * AbortObjCmd --
 *
 *      Implements loopctl_abort: abort a running thread using Tcl
 *      async signals.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
AbortObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    char           *id;
    Tcl_HashEntry  *hPtr;
    ThreadData     *threadPtr;
    int             result;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "thread-id");
        return TCL_ERROR;
    }
    id = Tcl_GetString(objv[1]);

    Ns_MutexLock(&lock);
    hPtr = Tcl_FindHashEntry(&threads, id);
    if (hPtr != NULL) {
        threadPtr = Tcl_GetHashValue(hPtr);
        Tcl_AsyncMark(threadPtr->cancel);
        result = TCL_OK;
    } else {
        Tcl_AppendResult(interp, "no such active thread: ", id, NULL);
        result = TCL_ERROR;
    }
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
 *      With the bytecode compiler, this procedure is only called when
 *      a command name is computed at runtime, and is "for" or the name
 *      to which "for" was renamed: e.g.,
 *      "set z for; $z {set i 0} {$i<100} {incr i} {puts $i}"
 *
 *      Copied from the Tcl source with additional calls to the
 *      loop control facility.
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
ForObjCmd(ClientData UNUSED(arg),  Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
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

                sprintf(msg, "\n    (\"for\" body line %d)",Tcl_GetErrorLine(interp));
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
 *      With the bytecode compiler, this procedure is only called when
 *      a command name is computed at runtime, and is "while" or the name
 *      to which "while" was renamed: e.g., "set z while; $z {$i<100} {}"
 *
 *      Copied from the Tcl source with additional calls to the
 *      loop control facility.
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
WhileObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
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
                        Tcl_GetErrorLine(interp));
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
ForeachObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
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
        argObjv = (Tcl_Obj **) ckalloc((size_t)objc * sizeof(Tcl_Obj *));
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
        index = (int *) ckalloc((size_t)numLists * sizeof(int));
        varcList = (int *) ckalloc((size_t)numLists * sizeof(int));
        varvList = (Tcl_Obj ***) ckalloc((size_t)numLists * sizeof(Tcl_Obj **));
        argcList = (int *) ckalloc((size_t)numLists * sizeof(int));
        argvList = (Tcl_Obj ***) ckalloc((size_t)numLists * sizeof(Tcl_Obj **));
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
                Tcl_Panic("nsloopctl: ForeachObjCmd: could not reconvert variable list %d to a list object\n", i);
            }
            result = Tcl_ListObjGetElements(interp, argObjv[2+i*2],
                                            &argcList[i], &argvList[i]);
            if (result != TCL_OK) {
                Tcl_Panic("nsloopctl: ForeachObjCmd: could not reconvert value list %d to a list object\n", i);
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
                        Tcl_GetErrorLine(interp));
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
 *      Loop can be monitored and possibly canceled by "loopctl_*".
 *
 *----------------------------------------------------------------------
 */

static void
EnterLoop(LoopData *loopPtr, int objc, Tcl_Obj * const objv[])
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
                Ns_TclLogErrorInfo(interp, "nsloopctl");
            }
            Ns_MutexLock(&lock);
            if (loopPtr->evalPtr == NULL) {
                Ns_Log(Error, "nsloopctl: dropped result: %s", Tcl_GetStringResult(interp));
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
        Tcl_SetObjResult(interp, Tcl_NewStringObj("nsloopctl: loop canceled: returning TCL_ERROR", -1));
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
ThreadAbort(ClientData UNUSED(clientData), Tcl_Interp *interp, int UNUSED(code))
{
    if (interp != NULL) {
        Tcl_ResetResult(interp);
        Tcl_SetObjResult(interp, Tcl_NewStringObj("nsloopctl: async thread abort: returning TCL_ERROR", -1));
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


/*
 *----------------------------------------------------------------------
 *
 * GetLoop --
 *
 *      Get the loop data struct given it's ID.
 *
 * Results:
 *      Pointer to LoopData or NULL if no such loop ID exists.
 *
 * Side effects:
 *      Tcl error message left as interp result.
 *
 *----------------------------------------------------------------------
 */

static LoopData *
GetLoop(Tcl_Interp *interp, Tcl_Obj *objPtr)
{
    char          *id;
    Tcl_HashEntry *hPtr;
    LoopData      *loopPtr;

    id = Tcl_GetString(objPtr);
    hPtr = Tcl_FindHashEntry(&loops, id);
    if (hPtr == NULL) {
        Tcl_AppendResult(interp, "no such loop id: ", id, NULL);
        return NULL;
    }
    loopPtr = Tcl_GetHashValue(hPtr);

    return loopPtr;
}


/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 70
 * indent-tabs-mode: nil
 * End:
 */
