/*
  insert.c:

  Copyright (C) 1991, 1997, 1999 2002
  Barry Vercoe, Istvan Varga, John ffitch,
  Gabriel Maldonado, matt ingalls

  This file is part of Csound.

  The Csound Library is free software; you can redistribute it
  and/or modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  Csound is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with Csound; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
  02111-1307 USA
*/

#include "cs.h"         /*                              INSERT.C        */
#include "insert.h"     /* for goto's */
#include "aops.h"       /* for cond's */
#include "midiops.h"
#include "namedins.h"   /* IV - Oct 31 2002 */

#ifdef mills_macintosh
#include "MacTransport.h"
#endif

/* accumulates the supposed percent of cpu usage */
static MYFLT cpu_power_busy=FL(0.0);
extern  OPARMS  O;

INSDS   *instance(int);

static  int     reinitflag = 0;         /* toggled by reinit for others */
static  OPDS    *ids, *pds;             /* used by init and perf loops  */
                                        /*  & modified by igoto, kgoto  */
void    showallocs(void);
extern  void    putop(TEXT*);
static  void    deact(INSDS *);
static  void    schedofftim(INSDS *);
int     sensOrcEvent(void);             /* For triginstr (re Aug 1999)  */
extern  int      csoundYield(void*);

void insertRESET(void)
{
    memset(&actanchor,0,sizeof(INSDS));
    curip          = NULL;
    frstoff        = NULL;
    kcounter       = 0;
    inerrcnt       = 0;
    perferrcnt     = 0;
    tieflag        = 0;
    reinitflag     = 0;
    ids            = NULL;
    pds            = NULL;
    cpu_power_busy = FL(0.0);
    /* don't forget local externs in this file... */
}

int init0(ENVIRON *csound)
{
    INSDS  *ip;

    curip = ip = instance(0);               /* allocate instr 0     */
    ids = (OPDS *)ip;
    while ((ids = ids->nxti) != NULL) {
      (*ids->iopadr)(csound,ids);           /*   run all i-code     */
    }
    return(inerrcnt);                       /*   return errcnt      */
}

void set_xtratim(ENVIRON *csound, INSDS *ip)
{
    sensEvents_t  *p;
    if (ip->relesing)
      return;
    p = &(csound->sensEvents_state);
    ip->offtim = p->curTime + (p->curTime_inc * (double) ip->xtratim);
    ip->offbet = p->curBeat + (p->curBeat_inc * (double) ip->xtratim);
    ip->relesing = 1;
}

int insert(ENVIRON *csound, int insno, EVTBLK *newevtp)
                                    /* insert an instr copy into active list */
{                                   /*      then run an init pass            */
    INSTRTXT *tp;
    INSDS  *ip, *prvp, *nxtp;
    int err = 0;

    if (O.odebug) printf("activating instr %d\n",insno);
    inerrcnt = 0;
    tp = instrtxtp[insno];
    if (tp->muted==0) {
      printf(Str("Instrument %d muted\n"), insno);
      /*       if (O.odebug) printf("Instrument %d muted\n", insno); */
      return 0;
    }
    if (tp->mdepends & 04) {
      printf(Str("instr %d expects midi event data, cannot run from score\n"),
             insno);
      return(1);
    }
    if ((ip = tp->instance) != NULL) { /* if allocs of text exist: */
      do {
        if (ip->insno == insno &&      /*   if find this insno,  */
            ip->actflg         &&      /*      active            */
            ip->offtim < 0.0   &&      /*      with indef (tie)  */
            ip->p1 == newevtp->p[1]) { /*      & matching p1     */
          tieflag++;
          goto init;                   /*     continue that event */
        }
      } while ((ip = ip->nxtinstance) != NULL);
      ip = tp->instance;              /*   else get alloc of text */
      do {
        if (!ip->actflg)              /*      that is free        */
          goto actlnk;                /*      and use its space   */
      } while ((ip = ip->nxtinstance) != NULL);
    }
    /* RWD: screen writes badly slow down RT playback */
    if (O.msglevel & 2) printf(Str("new alloc for instr %d:\n"),insno);
    ip = instance(insno);                   /* else alloc new dspace  */

 actlnk:
    cpu_power_busy += instrtxtp[insno]->cpuload;
    /* if there is no more cpu processing time*/
    if (cpu_power_busy > FL(100.0)) {
      cpu_power_busy -= instrtxtp[insno]->cpuload;
      csoundWarning(csound, Str("cannot allocate last note because it exceeds "
                                "100%% of cpu time"));
      return(0);
    }
    /* Add an active instrument */
    if (++instrtxtp[insno]->active > instrtxtp[insno]->maxalloc &&
        instrtxtp[insno]->maxalloc > 0) {
      instrtxtp[insno]->active--;
      csoundWarning(csound, Str("cannot allocate last note because it exceeds "
                                "instr maxalloc"));
      return(0);
    }

#ifdef TESTING
    printf("Now %d active instr %d\n", instrtxtp[insno]->active, insno);
#endif
    ip->insno = insno;
    nxtp = &actanchor;                      /* now splice into activ lst */
    while ((prvp = nxtp) && (nxtp = prvp->nxtact) != NULL) {
      if (nxtp->insno > insno ||
          (nxtp->insno == insno && nxtp->p1 > newevtp->p[1])) {
        nxtp->prvact = ip;
        break;
      }
    }
    ip->nxtact = nxtp;
    ip->prvact = prvp;
    prvp->nxtact = ip;
    ip->actflg++;                   /*    and mark the instr active */
    {
      int    n;
      MYFLT  *flp, *fep;

    init:
      if (tp->psetdata) {
        MYFLT *pfld = &ip->p3;              /* if pset data present */
        MYFLT *pdat = tp->psetdata + 2;
        long nn = tp->pmax - 2;             /*   put cur vals in pflds */
        do {
          *pfld++ = *pdat++;
        } while (nn--);
      }
      if ((n = tp->pmax) != newevtp->pcnt && !tp->psetdata) {
        csoundWarning(csound, Str("instr %d uses %d p-fields but is given %d"),
                              insno, n, newevtp->pcnt);
      }
      if (newevtp->p3orig >= FL(0.0))
        ip->offbet = csound->sensEvents_state.beatOffs
                     + (double) newevtp->p2orig + (double) newevtp->p3orig;
      else
        ip->offbet = -1.0;
      flp = &ip->p1;
      fep = &newevtp->p[1];
      if (O.odebug) printf("psave beg at %p\n",flp);
      if (n > newevtp->pcnt) n = newevtp->pcnt; /* IV - Oct 20 2002 */
      memcpy(flp, fep, n * sizeof(MYFLT)); flp += n;
      if (n < tp->pmax) memset(flp, 0, (tp->pmax - n) * sizeof(MYFLT));
      if (O.odebug) printf("   ending at %p\n",flp);
    }
    ip->offtim       = (double) ip->p3;         /* & duplicate p3 for now */
    ip->m_chnbp      = (MCHNBLK*) NULL;
    ip->xtratim      = 0;
    ip->relesing     = 0;
    ip->m_sust       = 0;
    ip->nxtolap      = NULL;
    ip->opcod_iobufs = NULL;
    curip            = ip;
    ids              = (OPDS *)ip;
    while ((ids = ids->nxti) != NULL) {   /* do init pass for this instr */
      if (O.odebug) printf("init %s:\n", opcodlst[ids->optext->t.opnum].opname);
      err |= (*ids->iopadr)(csound,ids);  /* $$$$ CHECK RETURN CODE $$$$ */
    }
    tieflag = 0;
    if (err || ip->p3 == FL(0.0)) {
      xturnoff_now(csound, ip);
      return(inerrcnt);
    }
    if (ip->p3 > FL(0.0) && ip->offtim > 0.0) { /* if still finite time, */
      double p2;
      p2 = (double) ip->p2 + csound->sensEvents_state.timeOffs;
      ip->offtim = p2 + (double) ip->p3;
      if (O.Beatmode) {
        p2 -= csound->sensEvents_state.curTime;
        p2 /= csound->sensEvents_state.beatTime;
        p2 += csound->sensEvents_state.curBeat;
        ip->offbet = p2 + ((double) ip->p3 / csound->sensEvents_state.beatTime);
      }
      schedofftim(ip);                          /*   put in turnoff list */
    }
    else {
      ip->offbet = -1.0;
      ip->offtim = -1.0;                        /*   else mark indef     */
    }
    if (O.odebug) {
      printf("instr %d now active:\n",insno); showallocs();
    }
    return(0);
}

int MIDIinsert(ENVIRON *csound, int insno, MCHNBLK *chn, MEVENT *mep)
/* insert a MIDI instr copy into active list */
/*  then run an init pass           */
{
    INSTRTXT *tp;
    INSDS    *ip, **ipp, *prvp, *nxtp;
    int      err = 0;

    if (insno <= 0 || instrtxtp[insno]->muted == 0)
      return 0;     /* muted */

    cpu_power_busy += instrtxtp[insno]->cpuload;
    if (cpu_power_busy > FL(100.0)) {   /* if there is no more cpu time*/
      cpu_power_busy -= instrtxtp[insno]->cpuload;
      csoundWarning(csound, Str("cannot allocate last note because it exceeds "
                                "100%% of cpu time"));
      return(0);
    }
    if (++instrtxtp[insno]->active > instrtxtp[insno]->maxalloc &&
        instrtxtp[insno]->maxalloc > 0) {
      instrtxtp[insno]->active--;
      csoundWarning(csound, Str("cannot allocate last note because it exceeds "
                                "instr maxalloc"));
      return(0);
    }
    if (O.odebug) printf("activating instr %d\n",insno);
    inerrcnt = 0;
    ipp = &chn->kinsptr[mep->dat1];       /* key insptr ptr           */
    tp = instrtxtp[insno];
    if (tp->instance != NULL) {           /* if allocs of text exist  */
      ip = tp->instance;                  /*    srch existing allocs  */
      do {
        if (!ip->actflg)                  /*      if one is free      */
          goto actlnk;                    /*      then use its space  */
      } while ((ip = ip->nxtinstance) != NULL);
    }
    printf(Str("new alloc for instr %d:\n"),insno);
    ip = instance(insno);                 /* else alloc new dspace  */
 actlnk:
    ip->insno = insno;
    if (O.odebug)
      printf("Now %d active instr %d\n", instrtxtp[insno]->active, insno);

    if ((prvp = *ipp) != NULL) {          /*   if key currently activ */
      csoundWarning(csound,
                    Str("MIDI note overlaps with key %d on same channel"),
                    (int) mep->dat1);
      while (prvp->nxtolap != NULL)       /*   append to overlap list */
        prvp = prvp->nxtolap;
      prvp->nxtolap = ip;
    }
    else
      *ipp = ip;
    /* of overlapping notes, the one that was turned on first will be */
    /* turned off first as well */
    ip->nxtolap = NULL;

    nxtp = &actanchor;                    /* now splice into activ lst */
    while ((prvp = nxtp) && (nxtp = prvp->nxtact) != NULL) {
      if (nxtp->insno > insno) {
        nxtp->prvact = ip;
        break;
      }
    }
    ip->nxtact = nxtp;
    ip->prvact = prvp;
    prvp->nxtact = ip;
    ip->actflg++;                         /* and mark the instr active */
    if (tp->pmax > 3 && tp->psetdata == NULL) {
      csoundWarning(csound, Str("instr %d p%d illegal for MIDI"),
                            insno, tp->pmax);
    }
    ip->m_chnbp = chn;                    /* rec address of chnl ctrl blk */
    ip->m_pitch = (unsigned char) mep->dat1;    /* rec MIDI data   */
    ip->m_veloc = (unsigned char) mep->dat2;
    ip->xtratim = 0;
    ip->m_sust = 0;
    ip->relesing = 0;
    ip->offbet = -1.0;
    ip->offtim = -1.0;              /* set indef duration */
    ip->opcod_iobufs = NULL;        /* IV - Sep 8 2002:            */
    ip->p1 = (MYFLT) ip->insno;     /* set these required p-fields */
    ip->p2 = (MYFLT) (csound->sensEvents_state.curTime
                      - csound->sensEvents_state.timeOffs);
    if (ip->p2 < FL(0.0))
      ip->p2 = FL(0.0);
    ip->p3 = FL(-1.0);
    if (tp->psetdata != NULL) {
      MYFLT *pfld = &ip->p3;              /* if pset data present */
      MYFLT *pdat = tp->psetdata + 2;
      long nn = tp->pmax - 2;             /*   put cur vals in pflds */
      do {
        *pfld++ = *pdat++;
      } while (nn--);
    }
    curip = ip;
    ids = (OPDS *)ip;
    while ((ids = ids->nxti) != NULL) { /* do init pass for this instr  */
      if (O.odebug) printf("init %s:\n", opcodlst[ids->optext->t.opnum].opname);
      err |= (*ids->iopadr)(csound,ids); /* $$$ CHECK RETURN CODE $$$ */
    }
    tieflag = 0;
    if (err) {
      xturnoff_now(csound, ip);
      return inerrcnt;
    }
    if (O.odebug) {
      printf("instr %d now active:\n",insno); showallocs();
    }
    return 0;
}

void showallocs(void)    /* debugging aid        */
{
    INSTRTXT *txtp;
    INSDS   *p;

    printf("insno\tinstanc\tnxtinst\tprvinst\tnxtact\t"
           "prvact\tnxtoff\tactflg\tofftim\n");
    for (txtp = &instxtanchor;  txtp != NULL;  txtp = txtp->nxtinstxt)
      if ((p = txtp->instance) != NULL) {
        /*
         * On Alpha, we print pointers as pointers.  heh 981101
         * and now on all platforms (JPff)
         */
        do {
          printf("%d\t%p\t%p\t%p\t%p\t%p\t%p\t%d\t%3.1f\n",
                 (int)p->insno, p,
                 p->nxtinstance,
                 p->prvinstance, p->nxtact,
                 p->prvact, p->nxtoff,
                 p->actflg, p->offtim);
        } while ((p = p->nxtinstance) != NULL);
      }
}

static void schedofftim(INSDS *ip)  /* put an active instr into offtime list  */
{                                   /* called by insert() & midioff + xtratim */
    INSDS *prvp, *nxtp;

    if ((nxtp = frstoff) == NULL ||
        nxtp->offtim > ip->offtim)          /*   set into       */
      frstoff = ip;                         /*   firstoff chain */
    else {
      while ((prvp = nxtp)
             && (nxtp = nxtp->nxtoff) != NULL
             && ip->offtim >= nxtp->offtim);
      prvp->nxtoff = ip;
    }
    ip->nxtoff = nxtp;
}

int useropcd(ENVIRON *, UOPCODE*);        /* IV - Oct 26 2002 */

static void deact(INSDS *ip)    /* unlink single instr from activ chain */
                                /*      and mark it inactive            */
{                               /*   close any files in fd chain        */
    INSDS  *nxtp;

    /* printf("active(%d) = %d\n", ip->insno, instrtxtp[ip->insno]->active); */
    instrtxtp[ip->insno]->active--;     /* remove an active instrument */
    cpu_power_busy -= instrtxtp[ip->insno]->cpuload;
    /* IV - Sep 8 2002: free subinstr instances */
    /* that would otherwise result in a memory leak */
    if (ip->opcod_deact) {
      UOPCODE *p = (UOPCODE*) ip->opcod_deact;          /* IV - Oct 26 2002 */
      deact(p->ip);             /* deactivate */
      /* link into free instance chain */
      p->ip->nxtact = instrtxtp[p->ip->insno]->act_instance;
      instrtxtp[p->ip->insno]->act_instance = p->ip;
      p->ip = NULL;
      /* IV - Oct 26 2002: set perf routine to "not initialised" */
      p->h.opadr = (SUBR) useropcd;
      ip->opcod_deact = NULL;
    }
    if (ip->subins_deact) {
      deact(((SUBINST*) ip->subins_deact)->ip);         /* IV - Oct 24 2002 */
      ((SUBINST*) ip->subins_deact)->ip = NULL;
      ip->subins_deact = NULL;
    }
    if (O.odebug) printf("removed instance of instr %d\n", ip->insno);
    /* IV - Oct 24 2002: ip->prvact may be NULL, so need to check */
    if (ip->prvact && (nxtp = ip->prvact->nxtact = ip->nxtact) != NULL)
      nxtp->prvact = ip->prvact;
    ip->actflg = 0;
    if (ip->fdch.nxtchp != NULL)
      fdchclose(ip);
}

int ihold(ENVIRON *csound, LINK *p)     /* make this note indefinit duration */
{                                       /* called by ihold statmnt at Itime  */
    if (!reinitflag) {                  /* no-op at reinit                   */
      curip->offbet = -1.0;
      curip->offtim = -1.0;
    }
    return OK;
}

/* Turn off a particular insalloc, also remove from list of active */
/* MIDI notes. Allows for releasing if ip->xtratim > 0. */

void xturnoff(ENVIRON *csound, INSDS *ip) /* turnoff a particular insalloc */
{                                         /* called by inexclus on ctrl 111 */
    MCHNBLK *chn;

    if (ip->relesing)
      return;                   /* already releasing: nothing to do */

    chn = ip->m_chnbp;
    if (chn != NULL) {                          /* if this was a MIDI note */
      INSDS *prvip;
      prvip = chn->kinsptr[ip->m_pitch];        /*    remov from activ lst */
      if (ip->m_sust && chn->ksuscnt)
        chn->ksuscnt--;
      ip->m_sust = 0;                   /* force turnoff even if sustaining */
      if (prvip != NULL) {
        if (prvip == ip)
          chn->kinsptr[ip->m_pitch] = ip->nxtolap;
        else {
          while (prvip != NULL && prvip->nxtolap != ip)
            prvip = prvip->nxtolap;
          if (prvip != NULL)
            prvip->nxtolap = ip->nxtolap;
        }
      }
    }
    /* remove from schedoff chain first if finite duration */
    if (frstoff != NULL && ip->offtim >= 0.0) {
      INSDS *prvip;
      prvip = frstoff;
      if (prvip == ip)
        frstoff = ip->nxtoff;
      else {
        while (prvip != NULL && prvip->nxtoff != ip)
          prvip = prvip->nxtoff;
        if (prvip != NULL)
          prvip->nxtoff = ip->nxtoff;
      }
    }
    /* if extra time needed: schedoff at new time */
    if (ip->xtratim > 0) {
      set_xtratim(csound, ip);
      schedofftim(ip);
    }
    else {
      /* no extra time needed: deactivate immediately */
      deact(ip);
    }
}

/* Turn off instrument instance immediately, without releasing. */
/* Removes alloc from list of active MIDI notes. */

void xturnoff_now(ENVIRON *csound, INSDS *ip)
{
    ip->xtratim = 0;
    ip->relesing = 0;
    xturnoff(csound, ip);
}

int turnoff(ENVIRON *csound, LINK *p)   /* terminate the current instrument  */
{                                       /* called by turnoff statmt at Ptime */
    INSDS  *lcurip = pds->insdshead;
    /* IV - Oct 16 2002: check for subinstr and user opcode */
    /* find top level instrument instance */
    while (lcurip->opcod_iobufs)
      lcurip = ((OPCOD_IOBUFS*) lcurip->opcod_iobufs)->parent_ip;
    xturnoff(csound, lcurip);
    if (lcurip->xtratim <= 0)
      while (pds->nxtp != NULL)
        pds = pds->nxtp;                /* loop to last opds */
    return OK;
}

void orcompact(ENVIRON *csound)         /* free all inactive instr spaces */
{
    INSTRTXT *txtp;
    INSDS   *ip, *nxtip, *prvip, **prvnxtloc;

    for (txtp = &instxtanchor;  txtp != NULL;  txtp = txtp->nxtinstxt) {
      if ((ip = txtp->instance) != NULL) {        /* if instance exists */
        prvip = NULL;
        prvnxtloc = &txtp->instance;
        do {
          if (ip->actflg == 0) {
            OPDS* off = ip->nxtp;
            while (off!=NULL) {
              if (off->dopadr) (*off->dopadr)(csound, off);
              off = off->nxtp;
            }

            /* SYY - 2003.11.30
             * call deinitialization on i-time opcodes
             */
            off = ip->nxti;
            while (off != NULL) {
              if(off->dopadr) (*off->dopadr)(csound, off);
              off = off->nxti;
            }

            if (ip->opcod_iobufs && ip->insno > maxinsno)
              mfree(csound, ip->opcod_iobufs);          /* IV - Nov 10 2002 */
            if (ip->fdch.nxtchp != NULL)
              fdchclose(ip);
            if (ip->auxch.nxtchp != NULL)
              auxchfree(csound, ip);
            if ((nxtip = ip->nxtinstance) != NULL)
              nxtip->prvinstance = prvip;
            *prvnxtloc = nxtip;
            mfree(csound, (char *)ip);
          }
          else {
            prvip = ip;
            prvnxtloc = &ip->nxtinstance;
          }
        }
        while ((ip = *prvnxtloc) != NULL);
      }
      /* IV - Oct 31 2002 */
      if (!txtp->instance)
        txtp->lst_instance = NULL;              /* find last alloc */
      else {
        ip = txtp->instance;
        while (ip->nxtinstance) ip = ip->nxtinstance;
        txtp->lst_instance = ip;
      }
      txtp->act_instance = NULL;                /* no free instances */
    }
    printf(Str("inactive allocs returned to freespace\n"));
}

void infoff(MYFLT p1)           /*  turn off an indef copy of instr p1  */
{                               /*      called by musmon                */
    INSDS *ip;
    int   insno;

    insno = (int)p1;
    if ((ip = (instrtxtp[insno])->instance) != NULL) {
      do {
        if (ip->insno == insno          /* if find the insno */
            && ip->actflg               /*      active       */
            && ip->offtim < 0.0         /*      but indef,   */
            && ip->p1 == p1) {
          if (O.odebug) printf("turning off inf copy of instr %d\n",insno);
          xturnoff(&cenviron, ip);
          return;                       /*      turn it off  */
        }
      } while ((ip = ip->nxtinstance) != NULL);
    }
    printf(Str("could not find indefinitely playing instr %d\n"),insno);
}

/* IV - Feb 05 2005: removed kcnt arg, as it is now always 1 */
/* also no return value, kperf always performs exactly one k-period */
void kperf(ENVIRON *csound)
         /* perform currently active instrs for one kperiod */
         /*      & send audio result to output buffer    */
{
    extern  void    (*spinrecv)(void*), (*spoutran)(void*);
    extern  void    (*nzerotran)(void*, long);
    INSDS  *ip;

/*  if (O.odebug)
      printf("perfing one kprd\n"); */

    /* PC GUI needs attention, but avoid excessively frequent */
    /* calls of csoundYield() */
    if (--(csound->evt_poll_cnt) < 0) {
      csound->evt_poll_cnt = csound->evt_poll_maxcnt;
      if (!csoundYield(csound))
        longjmp(csound->exitjmp_, CSOUND_EXITJMP_SUCCESS);
    }

    /* update orchestra time */
    kcounter++;
    global_kcounter = kcounter;
    csound->sensEvents_state.curTime += csound->sensEvents_state.curTime_inc;
    csound->sensEvents_state.curBeat += csound->sensEvents_state.curBeat_inc;

    if (!O.ksensing &&
        actanchor.nxtact == NULL) {     /* if !kreads & !instrs_activ, */
      (*nzerotran)(csound, 1L);         /*   send one zerospout */
    }
    else {                              /* else for one kcnt:      */
      if (O.sfread)                     /*   if audio_infile open  */
        (*spinrecv)(csound);            /*      fill the spin buf  */
      spoutactive = 0;                  /*   make spout inactive   */
      ip = &actanchor;
      while ((ip = ip->nxtact) != NULL) { /* for each instr active */
        pds = (OPDS *)ip;
        while ((pds = pds->nxtp) != NULL) {
          (*pds->opadr)(csound,pds);    /*      run each opcode    */
        }
      }
      if (spoutactive)                  /*   results now in spout? */
        (*spoutran)(csound);            /*      send to audio_out  */
      else
        (*nzerotran)(csound, 1L);       /*   else send zerospout   */
    }
}

int csoundInitError(void *csound, const char *s, ...)
{
    va_list args;
    INSDS   *ip;

    /* RWD: need this! */
    if (ids == NULL) {
      csoundMessage(csound, Str("\nINIT ERROR: "));
      va_start(args, s);
      csoundMessageV(csound, s, args);
      va_end(args);
      csoundMessage(csound, "\n");
      longjmp(((ENVIRON*) csound)->exitjmp_, 1);
    }
    /* IV - Oct 16 2002: check for subinstr and user opcode */
    ip = ids->insdshead;
    if (ip->opcod_iobufs) {
      OPCOD_IOBUFS *buf = (OPCOD_IOBUFS*) ip->opcod_iobufs;
      /* find top level instrument instance */
      while (ip->opcod_iobufs)
        ip = ((OPCOD_IOBUFS*) ip->opcod_iobufs)->parent_ip;
      if (buf->opcode_info)
        csoundMessage(csound, Str("INIT ERROR in instr %d (opcode %s): "),
                              ip->insno, buf->opcode_info->name);
      else
        csoundMessage(csound, Str("INIT ERROR in instr %d (subinstr %d): "),
                              ip->insno, ids->insdshead->insno);
    }
    else
      csoundMessage(csound, Str("INIT ERROR in instr %d: "), ip->insno);
    va_start(args, s);
    csoundMessageV(csound, s, args);
    va_end(args);
    csoundMessage(csound, "\n");
    putop(&ids->optext->t);

    return (++inerrcnt);
}

int csoundPerfError(void *csound, const char *s, ...)
{
    va_list args;
    INSDS   *ip;

    /* RWD and probably this too... */
    if (pds == NULL) {
      csoundMessage(csound, Str("\nPERF ERROR: "));
      va_start(args, s);
      csoundMessageV(csound, s, args);
      va_end(args);
      csoundMessage(csound, "\n");
      longjmp(((ENVIRON*) csound)->exitjmp_, 1);
    }
    /* IV - Oct 16 2002: check for subinstr and user opcode */
    ip = pds->insdshead;
    if (ip->opcod_iobufs) {
      OPCOD_IOBUFS *buf = (OPCOD_IOBUFS*) ip->opcod_iobufs;
      /* find top level instrument instance */
      while (ip->opcod_iobufs)
        ip = ((OPCOD_IOBUFS*) ip->opcod_iobufs)->parent_ip;
      if (buf->opcode_info)
        csoundMessage(csound, Str("PERF ERROR in instr %d (opcode %s): "),
                              ip->insno, buf->opcode_info->name);
      else
        csoundMessage(csound, Str("PERF ERROR in instr %d (subinstr %d): "),
                              ip->insno, pds->insdshead->insno);
    }
    else
      csoundMessage(csound, Str("PERF ERROR in instr %d: "), ip->insno);
    va_start(args, s);
    csoundMessageV(csound, s, args);
    va_end(args);
    csoundMessage(csound, "\n");
    putop(&pds->optext->t);
    csoundMessage(csound, Str("   note aborted\n"));
    perferrcnt++;
    xturnoff_now((ENVIRON*) csound, ip);      /* rm ins fr actlist */
    while (pds->nxtp != NULL)
      pds = pds->nxtp;                        /* loop to last opds */

    return perferrcnt;
}                                             /* contin from there */

int igoto(ENVIRON *csound, GOTO *p)
{
    ids = p->lblblk->prvi;
    return OK;
}

int kgoto(ENVIRON *csound, GOTO *p)
{
    pds = p->lblblk->prvp;
    return OK;
}

int icgoto(ENVIRON *csound, CGOTO *p)
{
    if (*p->cond)
      ids = p->lblblk->prvi;
    return OK;
}

int kcgoto(ENVIRON *csound, CGOTO *p)
{
    if (*p->cond)
      pds = p->lblblk->prvp;
    return OK;
}

/* an 'if-then' variant of 'if-goto' */
int ingoto(ENVIRON *csound, CGOTO *p)
{
    /* Make sure we have an i-time conditional */
    if (p->h.optext->t.intype == 'b' && !*p->cond)
      pds = p->lblblk->prvp;
    return OK;
}

int kngoto(ENVIRON *csound, CGOTO *p)
{
    if (!*p->cond)
      pds = p->lblblk->prvp;
    return OK;
}

int timset(ENVIRON *csound, TIMOUT *p)
{
    if ((p->cnt1 = (long)(*p->idel * ekr + FL(0.5))) < 0L
        || (p->cnt2 = (long)(*p->idur * ekr + FL(0.5))) < 0L)
      return csoundInitError(csound, Str("negative time period"));
    return OK;
}

int timout(ENVIRON *csound, TIMOUT *p)
{
    if (p->cnt1)                            /* once delay has expired, */
      p->cnt1--;
    else if (--p->cnt2 >= 0L)               /*  br during idur countdown */
      pds = p->lblblk->prvp;
    return OK;
}

int rireturn(ENVIRON *csound, LINK *p)
{
    IGN(p);
    return OK;
}

int reinit(ENVIRON *csound, GOTO *p)
{
    reinitflag = 1;
    curip = p->h.insdshead;
    ids = p->lblblk->prvi;        /* now, despite ANSI C warning:  */
    while ((ids = ids->nxti) != NULL && ids->iopadr != (SUBR)rireturn)
      (*ids->iopadr)(csound,ids);
    reinitflag = 0;
    return OK;
}

int rigoto(ENVIRON *csound, GOTO *p)
{
    if (reinitflag)
      ids = p->lblblk->prvi;
    return OK;
}

int tigoto(ENVIRON *csound, GOTO *p)    /* I-time only, NOP at reinit */
{
    if (tieflag && !reinitflag)
      ids = p->lblblk->prvi;
    return OK;
}

int tival(ENVIRON *csound, EVAL *p)     /* I-time only, NOP at reinit */
{
    if (!reinitflag)
      *p->r = (tieflag ? FL(1.0) : FL(0.0));
    return OK;
}

/* IV - Oct 12 2002: new simplified subinstr functions */

int subinstrset(ENVIRON *csound, SUBINST *p)
{
    OPDS    *saved_ids = ids;
    INSDS   *saved_curip = curip;
    MYFLT   *flp;
    int     instno, n, init_op, inarg_ofs;

    /* check if we are using subinstrinit or subinstr */
    init_op = (p->h.opadr == NULL ? 1 : 0);
    inarg_ofs = (init_op ? 0 : SUBINSTNUMOUTS);
    /* IV - Oct 31 2002 */
    if ((instno = strarg2insno(p->ar[inarg_ofs], p->STRARG)) < 0) return OK;
    /* IV - Oct 9 2002: need this check */
    if (!init_op && p->OUTOCOUNT > nchnls) {
      return csoundInitError(csound, Str("subinstr: number of output "
                                         "args greater than nchnls"));
    }
    /* IV - Oct 9 2002: copied this code from useropcdset() to fix some bugs */
    if (!(reinitflag | tieflag)) {
      /* search for already allocated, but not active instance */
      p->ip = instrtxtp[instno]->instance;
      while (p->ip && p->ip->actflg) p->ip = p->ip->nxtinstance;
      /* if none was found, allocate a new instance */
      if (p->ip == NULL) p->ip = instance(instno);
      p->ip->actflg++;                  /*    and mark the instr active */
      instrtxtp[instno]->active++;
      p->ip->p1 = p->ip->insno = instno;
      p->ip->opcod_iobufs = (void*) &p->buf;
      /* link into deact chain */
      p->ip->subins_deact = saved_curip->subins_deact;
      p->ip->opcod_deact = NULL;
      saved_curip->subins_deact = (void*) p;
      p->parent_ip = p->buf.parent_ip = saved_curip;
    }
    /* copy parameters from this instrument into our subinstrument */
    p->ip->xtratim = saved_curip->xtratim;
    p->ip->m_sust = 0;
    p->ip->relesing = saved_curip->relesing;
    p->ip->offbet = saved_curip->offbet;
    p->ip->offtim = saved_curip->offtim;
    p->ip->nxtolap = NULL;
    p->ip->p2 = saved_curip->p2;
    p->ip->p3 = saved_curip->p3;

    /* IV - Oct 31 2002 */
    p->ip->m_chnbp = saved_curip->m_chnbp;
    p->ip->m_pitch = saved_curip->m_pitch;
    p->ip->m_veloc = saved_curip->m_veloc;

    /* copy remainder of pfields        */
    flp = &p->ip->p3 + 1;
    /* by default all inputs are i-rate mapped to p-fields */
    if (p->INOCOUNT > (instrtxtp[instno]->pmax + 1)) {  /* IV - Nov 10 2002 */
      return csoundInitError(csound, Str("subinstr: too many p-fields"));
    }
    for (n = 1; n < p->INOCOUNT; n++)
      *flp++ = *p->ar[inarg_ofs + n];

    /* allocate memory for a temporary store of spout buffers */
    if (!init_op && !(reinitflag | tieflag))
      csoundAuxAlloc(csound, (long)nspout*sizeof(MYFLT), &p->saved_spout);

    /* do init pass for this instr */
    curip = p->ip;
    ids = (OPDS *)p->ip;

    while ((ids = ids->nxti) != NULL) {
      (*ids->iopadr)(csound,ids);
    }
    /* copy length related parameters back to caller instr */
    saved_curip->xtratim = curip->xtratim;
    saved_curip->relesing = curip->relesing;
    saved_curip->offbet = curip->offbet;
    saved_curip->offtim = curip->offtim;
    saved_curip->p3 = curip->p3;

    /* restore globals */
    ids = saved_ids;
    curip = saved_curip;
    return OK;
}

/* IV - Sep 8 2002: new functions for user defined opcodes (based */
/* on Matt J. Ingalls' subinstruments, but mostly rewritten) */

int useropcd1(ENVIRON *, UOPCODE*), useropcd2(ENVIRON *, UOPCODE*);

int useropcdset(ENVIRON *csound, UOPCODE *p)
{
    OPDS    *saved_ids = ids;
    INSDS   *saved_curip = curip, *parent_ip = curip, *lcurip;
    int     instno, i, n, pcnt;
    OPCODINFO *inm;
    OPCOD_IOBUFS  *buf;
    int     g_ksmps;
    MYFLT   g_ensmps, g_ekr, g_onedkr, g_hfkprd, g_kicvt;

    g_ksmps = p->l_ksmps = ksmps;       /* default ksmps */
    p->ksmps_scale = 1;
    /* look up the 'fake' instr number, and opcode name */
    inm = (OPCODINFO*) opcodlst[p->h.optext->t.opnum].useropinfo;
    instno = inm->instno;
    /* set local ksmps if defined by user */
    n = p->OUTOCOUNT + p->INOCOUNT - 1;
    if (*(p->ar[n]) != FL(0.0)) {
      i = (int) *(p->ar[n]);
      if (i < 1 || i > ksmps || ((ksmps / i) * i) != ksmps) {
        return csoundInitError(csound, Str("%s: invalid local ksmps value: %d"),
                                       inm->name, i);
      }
      p->l_ksmps = i;
    }
    /* save old globals */
    g_ensmps = ensmps;
    g_ekr = ekr; g_onedkr = onedkr; g_hfkprd = hfkprd; g_kicvt = kicvt;
    /* set up local variables depending on ksmps, also change globals */
    if (p->l_ksmps != g_ksmps) {
      ksmps = p->l_ksmps;
      p->ksmps_scale = g_ksmps / (int) ksmps;
      p->l_ensmps = ensmps = pool[O.poolcount + 2] = (MYFLT) p->l_ksmps;
      p->l_ekr = ekr = pool[O.poolcount + 1] = esr / p->l_ensmps;
      p->l_onedkr = onedkr = FL(1.0) / p->l_ekr;
      p->l_hfkprd = hfkprd = FL(0.5) / p->l_ekr;
      p->l_kicvt = kicvt = (MYFLT) FMAXLEN / p->l_ekr;
      kcounter *= p->ksmps_scale;
    }

    if (!(reinitflag | tieflag)) {
      /* search for already allocated, but not active instance */
      /* if none was found, allocate a new instance */
      if (!instrtxtp[instno]->act_instance)             /* IV - Oct 26 2002 */
        instance(instno);
      lcurip = instrtxtp[instno]->act_instance;     /* use free intance, and */
      instrtxtp[instno]->act_instance = lcurip->nxtact; /* remove from chain */
      lcurip->insno = instno;
      lcurip->actflg++;                 /*    and mark the instr active */
      instrtxtp[instno]->active++;
      /* link into deact chain */
      lcurip->opcod_deact = parent_ip->opcod_deact;
      lcurip->subins_deact = NULL;
      parent_ip->opcod_deact = (void*) p;
      p->ip = lcurip;
      /* IV - Nov 10 2002: set up pointers to I/O buffers */
      buf = p->buf = (OPCOD_IOBUFS*) lcurip->opcod_iobufs;
      buf->opcode_info = inm;
      /* initialise perf time address lists */
      buf->iobufp_ptrs[0] = buf->iobufp_ptrs[1] = NULL;
      buf->iobufp_ptrs[2] = buf->iobufp_ptrs[3] = NULL;
      /* store parameters of input and output channels, and parent ip */
      buf->uopcode_struct = (void*) p;
      buf->parent_ip = p->parent_ip = parent_ip;
    }

    /* copy parameters from the caller instrument into our subinstrument */
    lcurip = p->ip;
    lcurip->m_chnbp = parent_ip->m_chnbp;       /* MIDI parameters */
    lcurip->m_pitch = parent_ip->m_pitch;
    lcurip->m_veloc = parent_ip->m_veloc;
    lcurip->xtratim = parent_ip->xtratim * p->ksmps_scale;
    lcurip->m_sust = 0;
    lcurip->relesing = parent_ip->relesing;
    lcurip->offbet = parent_ip->offbet;
    lcurip->offtim = parent_ip->offtim;
    lcurip->nxtolap = NULL;
    /* copy all p-fields, including p1 (will this work ?) */
    if (instrtxtp[instno]->pmax > 3) {      /* requested number of p-fields */
      n = instrtxtp[instno]->pmax; pcnt = 0;
      while (pcnt < n) {
        if ((i = instrtxtp[parent_ip->insno]->pmax) > pcnt) {
          if (i > n) i = n;
          /* copy next block of p-fields */
          memcpy(&(lcurip->p1) + pcnt, &(parent_ip->p1) + pcnt,
                 (size_t) ((i - pcnt) * sizeof(MYFLT)));
          pcnt = i;
        }
        /* top level instr reached */
        if (parent_ip->opcod_iobufs == NULL) break;
        parent_ip = ((OPCOD_IOBUFS*) parent_ip->opcod_iobufs)->parent_ip;
      }
    }
    else memcpy(&(lcurip->p1), &(parent_ip->p1), 3 * sizeof(MYFLT));

    /* do init pass for this instr */
    curip = lcurip;
    ids = (OPDS *) (lcurip->nxti);
    while (ids != NULL) {
      (*ids->iopadr)(csound,ids);
      ids = ids->nxti;
    }
    /* copy length related parameters back to caller instr */
    if (ksmps == g_ksmps)
      saved_curip->xtratim = lcurip->xtratim;
    else
      saved_curip->xtratim = lcurip->xtratim / p->ksmps_scale;
    saved_curip->relesing = lcurip->relesing;
    saved_curip->offbet = lcurip->offbet;
    saved_curip->offtim = lcurip->offtim;
    saved_curip->p3 = lcurip->p3;

    /* restore globals */
    ids = saved_ids;
    curip = saved_curip;
    if (ksmps != g_ksmps) {
      ksmps = g_ksmps; ensmps = pool[O.poolcount + 2] = g_ensmps;
      ekr = pool[O.poolcount + 1] = g_ekr;
      onedkr = g_onedkr; hfkprd = g_hfkprd; kicvt = g_kicvt;
      kcounter = kcounter / p->ksmps_scale;
      /* IV - Sep 17 2002: also select perf routine */
      p->h.opadr = (SUBR) useropcd1;
    }
    else
      p->h.opadr = (SUBR) useropcd2;
    return OK;
}

/* IV - Sep 17 2002: dummy user opcode function for not initialised case */

int useropcd(ENVIRON *csound, UOPCODE *p)
{
    return csoundPerfError(csound, Str("%s: not initialised"),
                                   p->h.optext->t.opcod);
}

/* IV - Sep 1 2002: new opcodes: xin, xout */

int xinset(ENVIRON *csound, XIN *p)
{
    OPCOD_IOBUFS  *buf;
    OPCODINFO   *inm;
    short       *ndx_list;
    MYFLT       **tmp, **bufs;

    buf = (OPCOD_IOBUFS*) p->h.insdshead->opcod_iobufs;
    inm = buf->opcode_info;
    bufs = ((UOPCODE*) buf->uopcode_struct)->ar + inm->outchns;
    /* copy i-time variables */
    ndx_list = inm->in_ndx_list - 1;
    while (*++ndx_list >= 0)
      *(*(p->args + *ndx_list)) = *(*(bufs + *ndx_list));

    if (reinitflag | tieflag) return OK;
    /* find a-rate variables and add to list of perf-time buf ptrs ... */
    tmp = buf->iobufp_ptrs;
    if (*tmp || *(tmp + 1)) {
      return csoundInitError(csound, Str("xin was already used in this "
                                         "opcode definition"));
    }
    while (*++ndx_list >= 0) {
      *(tmp++) = *(bufs + *ndx_list);   /* "from" address */
      *(tmp++) = *(p->args + *ndx_list);/* "to" address */
    }
    *(tmp++) = NULL;            /* put delimiter */
    /* ... same for k-rate */
    while (*++ndx_list >= 0) {
      *(tmp++) = *(bufs + *ndx_list);   /* "from" address */
      *(tmp++) = *(p->args + *ndx_list);/* "to" address */
    }
    *(tmp++) = NULL;            /* put delimiter */
    /* fix for case when xout is omitted */
    *(tmp++) = NULL; *tmp = NULL;
    return OK;
}

int xoutset(ENVIRON *csound, XOUT *p)
{
    OPCOD_IOBUFS  *buf;
    OPCODINFO   *inm;
    short       *ndx_list;
    MYFLT       **tmp, **bufs;

    buf = (OPCOD_IOBUFS*) p->h.insdshead->opcod_iobufs;
    inm = buf->opcode_info;
    bufs = ((UOPCODE*) buf->uopcode_struct)->ar;
    /* copy i-time variables */
    ndx_list = inm->out_ndx_list - 1;
    while (*++ndx_list >= 0)
      *(*(bufs + *ndx_list)) = *(*(p->args + *ndx_list));

    if (reinitflag | tieflag) return OK;
    /* skip input pointers, including the two delimiter NULLs */
    tmp = buf->iobufp_ptrs;
    if (*tmp || *(tmp + 1)) tmp += (inm->perf_incnt << 1);
    tmp += 2;
    if (*tmp || *(tmp + 1)) {
      return csoundInitError(csound, Str("xout was already used in this "
                                         "opcode definition"));
    }
    /* find a-rate variables and add to list of perf-time buf ptrs ... */
    while (*++ndx_list >= 0) {
      *(tmp++) = *(p->args + *ndx_list);/* "from" address */
      *(tmp++) = *(bufs + *ndx_list);   /* "to" address */
    }
    *(tmp++) = NULL;            /* put delimiter */
    /* ... same for k-rate */
    while (*++ndx_list >= 0) {
      *(tmp++) = *(p->args + *ndx_list);/* "from" address */
      *(tmp++) = *(bufs + *ndx_list);   /* "to" address */
    }
    *tmp = NULL;                /* put delimiter */
    return OK;
}

/* IV - Sep 8 2002: new opcode: setksmps */

int setksmpsset(ENVIRON *csound, SETKSMPS *p)
{
    OPCOD_IOBUFS    *buf;
    UOPCODE *pp;
    int     l_ksmps, n;

    buf = (OPCOD_IOBUFS*) p->h.insdshead->opcod_iobufs;
    l_ksmps = (int) *(p->i_ksmps);
    if (!l_ksmps) return OK;       /* zero: do not change */
    if (l_ksmps < 1 || l_ksmps > ksmps
        || ((ksmps / l_ksmps) * l_ksmps != ksmps)) {
      return csoundInitError(csound, Str("setksmps: invalid ksmps value: %d"),
                                     l_ksmps);
    }
    /* set up global variables according to the new ksmps value */
    pp = (UOPCODE*) buf->uopcode_struct;
    n = ksmps / l_ksmps;
    pp->ksmps_scale *= n;
    p->h.insdshead->xtratim *= n;
    pp->l_ksmps = ksmps = l_ksmps;
    pp->l_ensmps = ensmps = pool[O.poolcount + 2] = (MYFLT) ksmps;
    pp->l_ekr = ekr = pool[O.poolcount + 1] = esr / ensmps;
    pp->l_onedkr = onedkr = FL(1.0) / ekr;
    pp->l_hfkprd = hfkprd = FL(0.5) / ekr;
    pp->l_kicvt = kicvt = (MYFLT) FMAXLEN / ekr;
    kcounter *= pp->ksmps_scale;
    return OK;
}

/* IV - Oct 16 2002: nstrnum opcode (returns the instrument number of a */
/* named instrument) */

int nstrnumset(ENVIRON *csound, NSTRNUM *p)
{
    /* IV - Oct 31 2002 */
    *(p->i_insno) = (MYFLT) strarg2insno(p->iname, p->STRARG);
    return OK;
}

/* IV - Nov 16 2002: moved insert_event() here to have access to some static */
/* variables defined in this file */

INSDS *insert_event(ENVIRON *csound,
                    MYFLT instr,
                    MYFLT when,
                    MYFLT dur,
                    int narg,
                    MYFLT **args,
                    int midi)
{
    int pcnt = narg + 3;
    int insno = (int) instr, saved_inerrcnt = inerrcnt;
    int saved_reinitflag = reinitflag, saved_tieflag = tieflag;
    INSDS *saved_curip = curip, *ip = NULL;
    INSDS *prvp, *nxtp;                                 /* IV - Nov 16 2002 */
    OPDS  *saved_ids = ids;
    INSTRTXT  *tp;

    printf("insert_event: %d %f %f ...\n", insno, when, dur);

    inerrcnt = tieflag = reinitflag = 0;        /* IV - Nov 16 2002 */
    if (instrtxtp[insno] == NULL) {
      printf(Str("schedule event ignored. instr %d undefined\n"), insno);
      perferrcnt++;
      goto endsched;            /* IV - Nov 16 2002 */
    }
    /* Insert this event into event queue */
    if (O.odebug) printf("activating instr %d\n",insno);
    tp = instrtxtp[insno];
    if ((tp->mdepends & 4) && !midi) {
      printf(Str("instr %d expects midi event data, cannot run from score\n"),
             insno);
      perferrcnt++;
      goto endsched;
    }
    if ((ip = tp->instance) != NULL) { /* if allocs of text exist: */
      do {
        if (ip->insno == insno        /*       if find this insno,  */
            && ip->actflg             /*        active              */
            && ip->offtim < 0.0       /*        with indef (tie)    */
            && ip->p1 == instr) {     /*  & matching p1             */
          tieflag++;
          goto init;                  /*        continue that event */
        }
      } while ((ip = ip->nxtinstance) != NULL);
      ip = tp->instance;              /*       else get alloc of text */
      do {
        if (!ip->actflg)              /*              that is free    */
          goto actlnk;                /*            and use its space */
      } while ((ip = ip->nxtinstance) != NULL);
    }

    if (O.msglevel & 2) printf(Str("new alloc for instr %d:\n"),insno);
    ip = instance(insno);     /* else alloc new dspace  */

 actlnk:
    cpu_power_busy += tp->cpuload;
    if (cpu_power_busy > 100.0) { /* if there is no more cpu processing time*/
      cpu_power_busy -= tp->cpuload;
      csoundWarning(csound, Str("cannot allocate last note because it exceeds "
                                "100%% of cpu time"));
      ip = NULL; goto endsched;
    }
    /* Add an active instrument */
    if (++tp->active > tp->maxalloc && tp->maxalloc > 0) {
      tp->active--;
      csoundWarning(csound, Str("cannot allocate last note because it exceeds "
                                "instr maxalloc"));
      ip = NULL; goto endsched;
    }
    ip->insno = insno;
    nxtp = &actanchor;        /* now splice into active list */
    while ((prvp = nxtp) && (nxtp = prvp->nxtact) != NULL)
      if (nxtp->insno > insno    ||
          (nxtp->insno == insno && nxtp->p1 > instr)) {
        nxtp->prvact = ip;
        break;
      }
    ip->nxtact = nxtp;
    ip->prvact = prvp;
    prvp->nxtact = ip;
    ip->actflg++;             /*        and mark the instr active */
 init:
    {
      int i;
      int imax = tp->pmax - 3;
      MYFLT  *flp;
      if ((int) tp->pmax != pcnt) {
        csoundWarning(csound, Str("instr %d pmax = %d, note pcnt = %d"),
                              insno, (int) tp->pmax, pcnt);
      }
      ip->p1 = instr;
      ip->p2 = when;
      ip->p3 = dur;
      flp = &(ip->p1) + 3;
      if (O.odebug) printf(Str("psave beg at %p\n"),flp);
      for (i = 0; i < imax; i++) {
        if (i < narg)
          *flp++ = *(args[i]);
        else
          *flp++ = FL(0.0);
      }
      if (O.odebug) printf(Str("   ending at %p\n"),flp);
    }
    ip->offbet = (double) ip->p3;
    ip->offtim = (double) ip->p3;       /* & duplicate p3 for now */
    ip->xtratim = 0;
    ip->relesing = 0;
    ip->m_sust = 0;
    ip->nxtolap = NULL;
    /* IV - Nov 16 2002 */
    ip->opcod_iobufs = NULL;
    if (midi) {
      /* should we copy MIDI parameters from the note from which the */
      /* event was scheduled ? */
      ip->m_chnbp = saved_curip->m_chnbp;
      ip->m_pitch = saved_curip->m_pitch;
      ip->m_veloc = saved_curip->m_veloc;
    }
    else
      ip->m_chnbp = NULL;     /* score event */
    curip = ip;
    ids = (OPDS *)ip;
    while ((ids = ids->nxti) != NULL) {  /* do init pass for this instr */
      /*    if (O.odebug) printf("init %s:\n",
            opcodlst[ids->optext->t.opnum].opname);      */
      (*ids->iopadr)(csound,ids);
    }
    if (inerrcnt || ip->p3 == FL(0.0)) {
      xturnoff_now(csound, ip);
      ip = NULL; goto endsched;
    }
    if (!midi &&                /* if not MIDI activated, */
        ip->p3 > FL(0.0)) {     /* and still finite time, */
      double p2;
      p2 = (double) ip->p2 + csound->sensEvents_state.timeOffs;
      ip->offtim = p2 + (double) ip->p3;
      p2 -= csound->sensEvents_state.curTime;
      p2 /= csound->sensEvents_state.beatTime;
      p2 += csound->sensEvents_state.curBeat;
      ip->offbet = p2 + ((double) ip->p3 / csound->sensEvents_state.beatTime);
      schedofftim(ip);          /*       put in turnoff list */
    }
    else {
      ip->offbet = -1.0;
      ip->offtim = -1.0;        /* else mark indef */
    }
    if (O.odebug) {
      printf("instr %d now active:\n",insno); showallocs();
    }
 endsched:
    /* IV - Nov 16 2002: restore globals */
    inerrcnt = saved_inerrcnt;
    reinitflag = saved_reinitflag;
    tieflag = saved_tieflag;
    curip = saved_curip;
    ids = saved_ids;
    return ip;
}

/* IV - Feb 05 2005: changed to double */

void beatexpire(ENVIRON *csound, double beat)
                                /* unlink expired notes from activ chain */
{                               /*      and mark them inactive          */
    INSDS  *ip;                 /*    close any files in each fdchain   */
 strt:
    if ((ip = frstoff) != NULL && ip->offbet <= beat) {
      do {
        if (!ip->relesing && ip->xtratim) {
          /* IV - Nov 30 2002: allow extra time for finite length (p3 > 0) */
          /* score notes */
          set_xtratim(csound, ip);      /* enter release stage */
          frstoff = ip->nxtoff;         /* update turnoff list */
          schedofftim(ip);
          goto strt;                    /* and start again */
        }
        else
          deact(ip);    /* IV - Sep 5 2002: use deact() as it also */
      }                 /* deactivates subinstrument instances */
      while ((ip = ip->nxtoff) != NULL && ip->offbet <= beat);
      frstoff = ip;
      if (O.odebug) {
        printf("deactivated all notes to beat %7.3f\n",beat);
        printf("frstoff = %p\n",frstoff);
      }
    }
}

/* IV - Feb 05 2005: changed to double */

void timexpire(ENVIRON *csound, double time)
                                /* unlink expired notes from activ chain */
{                               /*      and mark them inactive           */
    INSDS  *ip;                 /*    close any files in each fdchain    */

 strt:
    if ((ip = frstoff) != NULL && ip->offtim <= time) {
      do {
        if (!ip->relesing && ip->xtratim) {
          /* IV - Nov 30 2002: allow extra time for finite length (p3 > 0) */
          /* score notes */
          set_xtratim(csound, ip);      /* enter release stage */
          frstoff = ip->nxtoff;         /* update turnoff list */
          schedofftim(ip);
          goto strt;                    /* and start again */
        }
        else
          deact(ip);    /* IV - Sep 5 2002: use deact() as it also */
      }                 /* deactivates subinstrument instances */
      while ((ip = ip->nxtoff) != NULL && ip->offtim <= time);
      frstoff = ip;
      if (O.odebug) {
        printf("deactivated all notes to time %7.3f\n",time);
        printf("frstoff = %p\n",frstoff);
      }
    }
}

int subinstr(ENVIRON *csound, SUBINST *p)
{
    OPDS    *saved_pds = pds;
    int     saved_sa = spoutactive;
    MYFLT   *pbuf, *saved_spout = spout;
    long    frame, chan;

    if (p->ip == NULL) {                /* IV - Oct 26 2002 */
      return csoundPerfError(csound, Str("subinstr: not initialised"));
    }
    /* copy current spout buffer and clear it */
    spout = (MYFLT*) p->saved_spout.auxp;
    spoutactive = 0;
    /* update release flag */
    p->ip->relesing = p->parent_ip->relesing;   /* IV - Nov 16 2002 */

    /*  run each opcode  */
    pds = (OPDS *)p->ip;
    while ((pds = pds->nxtp) != NULL) {
      (*pds->opadr)(csound,pds);
    }

    /* copy outputs */
    for (chan = 0; chan < p->OUTOCOUNT; chan++) {
      for (pbuf = spout + chan, frame = 0; frame < ksmps; frame++) {
        p->ar[chan][frame] = *pbuf;
        pbuf += nchnls;
      }
    }

    /* restore spouts */
    spout = saved_spout;
    spoutactive = saved_sa;
    pds = saved_pds;
    /* check if instrument was deactivated (e.g. by perferror) */
    if (!p->ip)
      while (pds->nxtp) pds = pds->nxtp;        /* loop to last opds */
    return OK;
}

/* IV - Sep 17 2002 -- case 1: local ksmps is used */

int useropcd1(ENVIRON *csound, UOPCODE *p)
{
    OPDS    *saved_pds = pds;
    int     g_ksmps, ofs = 0, n;
    MYFLT   g_ensmps, g_ekr, g_onedkr, g_hfkprd, g_kicvt, **tmp, *ptr1, *ptr2;
    long    g_kcounter;

    /* update release flag */
    p->ip->relesing = p->parent_ip->relesing;   /* IV - Nov 16 2002 */
    /* save old globals */
    g_ksmps = ksmps; g_ensmps = ensmps;
    g_ekr = ekr; g_onedkr = onedkr; g_hfkprd = hfkprd; g_kicvt = kicvt;
    g_kcounter = kcounter;
    /* set local ksmps and related values */
    ksmps = p->l_ksmps; ensmps = pool[O.poolcount + 2] = p->l_ensmps;
    ekr = pool[O.poolcount + 1] = p->l_ekr;
    onedkr = p->l_onedkr; hfkprd = p->l_hfkprd; kicvt = p->l_kicvt;
    kcounter = kcounter * p->ksmps_scale;

    if (ksmps == 1) {                   /* special case for local kr == sr */
      do {
        /* copy inputs */
        tmp = p->buf->iobufp_ptrs;
        while (*tmp) {                  /* a-rate */
          ptr1 = *(tmp++) + ofs; *(*(tmp++)) = *ptr1;
        }
        while (*(++tmp)) {              /* k-rate */
          ptr1 = *tmp; *(*(++tmp)) = *ptr1;
        }
        /*  run each opcode  */
        pds = (OPDS *) (p->ip);
        while ((pds = pds->nxtp)) {
          (*pds->opadr)(csound,pds);
        }
        /* copy outputs */
        while (*(++tmp)) {              /* a-rate */
          ptr1 = *tmp; (*(++tmp))[ofs] = *ptr1;
        }
        ++kcounter;
      } while (++ofs < g_ksmps);
    }
    else {                              /* generic case for local kr != sr */
      do {
        /* copy inputs */
        tmp = p->buf->iobufp_ptrs;
        while (*tmp) {                  /* a-rate */
          ptr1 = *(tmp++) + ofs; ptr2 = *(tmp++);
          n = ksmps;
          do {
            *(ptr2++) = *(ptr1++);
          } while (--n);
        }
        while (*(++tmp)) {              /* k-rate */
          ptr1 = *tmp; *(*(++tmp)) = *ptr1;
        }
        /*  run each opcode  */
        pds = (OPDS *) (p->ip);
        while ((pds = pds->nxtp)) {
          (*pds->opadr)(csound,pds);
        }
        /* copy outputs */
        while (*(++tmp)) {              /* a-rate */
          ptr1 = *tmp; ptr2 = *(++tmp) + ofs;
          n = ksmps;
          do {
            *(ptr2++) = *(ptr1++);
          } while (--n);
        }
        ++kcounter;
      } while ((ofs += ksmps) < g_ksmps);
    }
    /* k-rate outputs are copied only in the last sub-kperiod, */
    /* so we do it now */
    while (*(++tmp)) {                  /* k-rate */
      ptr1 = *tmp; *(*(++tmp)) = *ptr1;
    }

    /* restore globals */
    ksmps = g_ksmps; ensmps = pool[O.poolcount + 2] = g_ensmps;
    ekr = pool[O.poolcount + 1] = g_ekr;
    onedkr = g_onedkr; hfkprd = g_hfkprd; kicvt = g_kicvt;
    kcounter = g_kcounter;
    pds = saved_pds;
    /* check if instrument was deactivated (e.g. by perferror) */
    if (!p->ip)
      while (pds->nxtp) pds = pds->nxtp;    /* loop to last opds */
    return OK;
}

/* IV - Sep 17 2002 -- case 2: simplified routine for no local ksmps */

int useropcd2(ENVIRON *csound, UOPCODE *p)
{
    OPDS    *saved_pds = pds;
    int     n;
    MYFLT   **tmp, *ptr1, *ptr2;

    if (!(pds = (OPDS*) (p->ip->nxtp))) goto endop;     /* no perf code */
    /* IV - Nov 16 2002: update release flag */
    p->ip->relesing = p->parent_ip->relesing;

    tmp = p->buf->iobufp_ptrs;
    if (ksmps != 1) {                   /* generic case for kr != sr */
      /* copy inputs */
      while (*tmp) {                    /* a-rate */
        ptr1 = *(tmp++); ptr2 = *(tmp++);
        n = ksmps;
        do {
          *(ptr2++) = *(ptr1++);
        } while (--n);
      }
      while (*(++tmp)) {                /* k-rate */
        ptr1 = *tmp; *(*(++tmp)) = *ptr1;
      }
      /*  run each opcode  */
      do {
        (*pds->opadr)(csound,pds);
      } while ((pds = pds->nxtp));
      /* copy outputs */
      while (*(++tmp)) {                /* a-rate */
        ptr1 = *tmp; ptr2 = *(++tmp);
        n = ksmps;
        do {
          *(ptr2++) = *(ptr1++);
        } while (--n);
      }
    }
    else {                      /* special case for kr == sr */
      /* copy inputs */
      while (*tmp) {                    /* a-rate */
        ptr1 = *(tmp++); *(*(tmp++)) = *ptr1;
      }
      while (*(++tmp)) {                /* k-rate */
        ptr1 = *tmp; *(*(++tmp)) = *ptr1;
      }
      /*  run each opcode  */
      do {
        (*pds->opadr)(csound,pds);
      } while ((pds = pds->nxtp));
      /* copy outputs */
      while (*(++tmp)) {                /* a-rate */
        ptr1 = *tmp; *(*(++tmp)) = *ptr1;
      }
    }
    while (*(++tmp)) {                  /* k-rate */
      ptr1 = *tmp; *(*(++tmp)) = *ptr1;
    }
 endop:
    /* restore globals */
    pds = saved_pds;
    /* check if instrument was deactivated (e.g. by perferror) */
    if (!p->ip)
      while (pds->nxtp) pds = pds->nxtp;    /* loop to last opds */
    return OK;
}

/* turnoff2 opcode */

int turnoff2(ENVIRON *csound, TURNOFF2 *p)
{
    MYFLT p1;
    INSDS *ip, *ip2;
    int   mode, insno, allow_release;

    if (*(p->kInsNo) <= FL(0.0))
      return OK;    /* not triggered */
    p1 = *(p->kInsNo);
    insno = (int) p1;
    if (insno < 1 || insno > (int) maxinsno || instrtxtp[insno] == NULL) {
      csoundPerfError(csound, Str("turnoff2: invalid instrument number"));
      return NOTOK;
    }
    mode = (int) (*(p->kFlags) + FL(0.5));
    allow_release = (*(p->kRelease) == FL(0.0) ? 0 : 1);
    if (mode < 0 || mode > 15 || (mode & 3) == 3) {
      csoundPerfError(csound, Str("turnoff2: invalid mode parameter"));
      return NOTOK;
    }
    ip = &actanchor;
    ip2 = NULL;
    while ((ip = ip->nxtact) != NULL && (int) ip->insno != insno);
    if (ip == NULL)
      return OK;
    do {
      if (((mode & 8) && ip->offtim >= 0.0) ||
          ((mode & 4) && ip->p1 != p1) ||
          (allow_release && ip->relesing))
        continue;
      if (!(mode & 3)) {
        if (allow_release)
          xturnoff(csound, ip);
        else
          xturnoff_now(csound, ip);
      }
      else {
        ip2 = ip;
        if ((mode & 3) == 1)
          break;
      }
    } while ((ip = ip->nxtact) != NULL && (int) ip->insno == insno);
    if (ip2 != NULL) {
      if (allow_release)
        xturnoff(csound, ip2);
      else
        xturnoff_now(csound, ip2);
    }
    if (!p->h.insdshead->actflg) {  /* if current note was deactivated: */
      while (pds->nxtp != NULL)
        pds = pds->nxtp;            /* loop to last opds */
    }
    return OK;
}

