/***************************************************************************
    begin       : Mon Mar 01 2004
    copyright   : (C) 2004-2010 by Martin Preuss
    email       : martin@libchipcard.de

 ***************************************************************************
 *          Please see toplevel file COPYING for license details           *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "provider_p.h"
#include "aqhbci_l.h"
#include "account_l.h"
#include "hbci_l.h"
#include "dialog_l.h"
#include "outbox_l.h"
#include "user_l.h"

#include "jobgetbalance_l.h"
#include "jobgettransactions_l.h"
#include "jobgetstandingorders_l.h"
#include "jobgetdatedxfers_l.h"
#include "jobsingletransfer_l.h"
#include "jobmultitransfer_l.h"
#include "jobeutransfer_l.h"
#include "jobloadcellphone_l.h"

/* special jobs */
#include "jobforeignxferwh_l.h"

#include "dlg_newuser_l.h"
#include "dlg_pintan_l.h"
#include "dlg_ddvcard_l.h"
#include "dlg_newkeyfile_l.h"
#include "dlg_importkeyfile_l.h"
#include "dlg_edituserpintan_l.h"
#include "dlg_edituserddv_l.h"
#include "dlg_edituserrdh_l.h"
#include "dlg_choose_usertype_l.h"

#include "adminjobs_l.h"
#include <aqhbci/user.h>

#include <aqbanking/banking_be.h>
#include <aqbanking/account_be.h>
#include <aqbanking/provider_be.h>
#include <aqbanking/job_be.h>

#include <gwenhywfar/misc.h>
#include <gwenhywfar/inherit.h>
#include <gwenhywfar/directory.h>
#include <gwenhywfar/text.h>
#include <gwenhywfar/debug.h>
#include <gwenhywfar/gui.h>
#include <gwenhywfar/mdigest.h>
#include <gwenhywfar/ctplugin.h>

#include <ctype.h>
#include <stdlib.h>


#ifdef OS_WIN32
# define AH_PATH_SEP "\\"
#else
# define AH_PATH_SEP "/"
#endif



GWEN_INHERIT(AB_PROVIDER, AH_PROVIDER);


AB_PROVIDER *AH_Provider_new(AB_BANKING *ab, const char *name){
  AB_PROVIDER *pro;
  AH_PROVIDER *hp;
  GWEN_BUFFER *pbuf;

  pbuf=0;
  pro=AB_Provider_new(ab, name);
  assert(pro);

  AB_Provider_SetInitFn(pro, AH_Provider_Init);
  AB_Provider_SetFiniFn(pro, AH_Provider_Fini);
  AB_Provider_SetUpdateJobFn(pro, AH_Provider_UpdateJob);
  AB_Provider_SetAddJobFn(pro, AH_Provider_AddJob);
  AB_Provider_SetExecuteFn(pro, AH_Provider_Execute);
  AB_Provider_SetResetQueueFn(pro, AH_Provider_ResetQueue);
  AB_Provider_SetExtendUserFn(pro, AH_Provider_ExtendUser);
  AB_Provider_SetExtendAccountFn(pro, AH_Provider_ExtendAccount);
  AB_Provider_SetUpdateFn(pro, AH_Provider_Update);
  AB_Provider_SetGetNewUserDialogFn(pro, AH_Provider_GetNewUserDialog);
  AB_Provider_SetGetEditUserDialogFn(pro, AH_Provider_GetEditUserDialog);
  AB_Provider_SetGetUserTypeDialogFn(pro, AH_Provider_GetUserTypeDialog);

  AB_Provider_AddFlags(pro,
		       AB_PROVIDER_FLAGS_HAS_NEWUSER_DIALOG |
		       AB_PROVIDER_FLAGS_HAS_EDITUSER_DIALOG |
		       AB_PROVIDER_FLAGS_HAS_USERTYPE_DIALOG);

  GWEN_NEW_OBJECT(AH_PROVIDER, hp);
  GWEN_INHERIT_SETDATA(AB_PROVIDER, AH_PROVIDER, pro, hp,
                       AH_Provider_FreeData);

  hp->hbci=AH_HBCI_new(pro);
  assert(hp->hbci);
  GWEN_Buffer_free(pbuf);

  hp->dbTempConfig=GWEN_DB_Group_new("tmpConfig");
  hp->bankingJobs=AB_Job_List2_new();

  return pro;
}



void GWENHYWFAR_CB AH_Provider_FreeData(void *bp, void *p) {
  AH_PROVIDER *hp;

  DBG_INFO(AQHBCI_LOGDOMAIN, "Destroying AH_PROVIDER");
  hp=(AH_PROVIDER*)p;
  AB_Job_List2_FreeAll(hp->bankingJobs);
  AH_Outbox_free(hp->outbox);

  GWEN_DB_Group_free(hp->dbTempConfig);
  AH_HBCI_free(hp->hbci);

  GWEN_FREE_OBJECT(hp);
}



int AH_Provider_Init(AB_PROVIDER *pro, GWEN_DB_NODE *dbData) {
  AH_PROVIDER *hp;
  int rv;
  const char *logLevelName;

  if (!GWEN_Logger_IsOpen(AQHBCI_LOGDOMAIN)) {
    GWEN_Logger_Open(AQHBCI_LOGDOMAIN,
		     "aqhbci", 0,
		     GWEN_LoggerType_Console,
		     GWEN_LoggerFacility_User);
  }

  logLevelName=getenv("AQHBCI_LOGLEVEL");
  if (logLevelName) {
    GWEN_LOGGER_LEVEL ll;

    ll=GWEN_Logger_Name2Level(logLevelName);
    if (ll!=GWEN_LoggerLevel_Unknown) {
      GWEN_Logger_SetLevel(AQHBCI_LOGDOMAIN, ll);
      DBG_WARN(AQHBCI_LOGDOMAIN,
               "Overriding loglevel for AqHBCI with \"%s\"",
               logLevelName);
    }
    else {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Unknown loglevel \"%s\"",
                logLevelName);
    }
  }

  DBG_NOTICE(AQHBCI_LOGDOMAIN, "Initializing AqHBCI backend");
  assert(pro);

  hp=GWEN_INHERIT_GETDATA(AB_PROVIDER, AH_PROVIDER, pro);
  assert(hp);

  GWEN_DB_ClearGroup(hp->dbTempConfig, 0);

  rv=AH_HBCI_Init(hp->hbci, dbData);
  if (rv<0) {
    DBG_INFO(AQHBCI_LOGDOMAIN, "here (%d)", rv);
  }

  return rv;
}



int AH_Provider_Fini(AB_PROVIDER *pro, GWEN_DB_NODE *dbData) {
  AH_PROVIDER *hp;
  int rv;

  DBG_NOTICE(AQHBCI_LOGDOMAIN, "Deinitializing AqHBCI backend");

  assert(pro);
  hp=GWEN_INHERIT_GETDATA(AB_PROVIDER, AH_PROVIDER, pro);
  assert(hp);

  AB_Job_List2_FreeAll(hp->bankingJobs);
  hp->bankingJobs=AB_Job_List2_new();
  AH_Outbox_free(hp->outbox);
  hp->outbox=0;

  rv=AH_HBCI_Fini(hp->hbci, dbData);
  GWEN_DB_ClearGroup(hp->dbTempConfig, 0);

  return rv;
}



const char *AH_Provider_GetProductName(const AB_PROVIDER *pro) {
  AH_HBCI *h;

  assert(pro);
  h=AH_Provider_GetHbci(pro);
  assert(h);
  return AH_HBCI_GetProductName(h);
}



const char *AH_Provider_GetProductVersion(const AB_PROVIDER *pro) {
  AH_HBCI *h;

  assert(pro);
  h=AH_Provider_GetHbci(pro);
  assert(h);
  return AH_HBCI_GetProductVersion(h);
}



int AH_Provider_CheckCryptToken(AB_PROVIDER *pro,
				GWEN_CRYPT_TOKEN_DEVICE devt,
				GWEN_BUFFER *typeName,
				GWEN_BUFFER *tokenName) {
  GWEN_PLUGIN_MANAGER *pm;
  int rv;

  /* get crypt token */
  pm=GWEN_PluginManager_FindPluginManager(GWEN_CRYPT_TOKEN_PLUGIN_TYPENAME);
  if (pm==0) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "CryptToken plugin manager not found");
    return GWEN_ERROR_NOT_FOUND;
  }

  /* try to determine the type and name */
  rv=GWEN_Crypt_Token_PluginManager_CheckToken(pm,
					       devt,
					       typeName,
					       tokenName,
					       0);
  if (rv) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "here (%d)", rv);
    return rv;
  }

  return 0;
}



int AH_Provider_UpdateJob(AB_PROVIDER *pro, AB_JOB *j){
  AH_PROVIDER *hp;
  GWEN_DB_NODE *dbJob;
  AH_JOB *mj;
  AB_USER *mu;
  AB_ACCOUNT *ma;
  int rv;
  uint32_t uFlags;
  uint32_t aFlags;

  assert(pro);
  hp=GWEN_INHERIT_GETDATA(AB_PROVIDER, AH_PROVIDER, pro);
  assert(hp);

  dbJob=AB_Job_GetProviderData(j, pro);
  assert(dbJob);

  ma=AB_Job_GetAccount(j);
  assert(ma);

  /* determine customer to use */
  mu=AB_Account_GetFirstUser(ma);
  if (!mu) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "No customer for this account");
    return GWEN_ERROR_NOT_AVAILABLE;
  }

  uFlags=AH_User_GetFlags(mu);
  aFlags=AH_Account_GetFlags(ma);

  mj=0;
  switch(AB_Job_GetType(j)) {

  case AB_Job_TypeGetBalance:
    mj=AH_Job_GetBalance_new(mu, ma);
    if (!mj) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Job not supported with this account");
      return GWEN_ERROR_NOT_AVAILABLE;
    }
    break;

  case AB_Job_TypeGetTransactions:
    mj=AH_Job_GetTransactions_new(mu, ma);
    if (!mj) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Job not supported with this account");
      return GWEN_ERROR_NOT_AVAILABLE;
    }
    break;

  case AB_Job_TypeGetStandingOrders:
    mj=AH_Job_GetStandingOrders_new(mu, ma);
    if (!mj) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Job not supported with this account");
      return GWEN_ERROR_NOT_AVAILABLE;
    }
    break;

  case AB_Job_TypeGetDatedTransfers:
    mj=AH_Job_GetDatedTransfers_new(mu, ma);
    if (!mj) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Job not supported with this account");
      return GWEN_ERROR_NOT_AVAILABLE;
    }
    break;

  case AB_Job_TypeTransfer:
    mj=0;
    if (!(aFlags & AH_BANK_FLAGS_PREFER_SINGLE_TRANSFER)) {
      DBG_INFO(AQHBCI_LOGDOMAIN, "Customer prefers multi jobs");
      /* create new job */
      mj=AH_Job_MultiTransfer_new(mu, ma);
      if (!mj) {
	DBG_WARN(AQHBCI_LOGDOMAIN,
		 "Multi-job not supported with this account, "
		 "using single-job");
      }
    }
    else {
      DBG_INFO(AQHBCI_LOGDOMAIN, "Customer prefers single jobs");
    }

    if (mj) {
      GWEN_DB_SetIntValue(dbJob, GWEN_DB_FLAGS_OVERWRITE_VARS,
			  "isMultiJob", 1);
    }
    else {
      GWEN_DB_SetIntValue(dbJob, GWEN_DB_FLAGS_OVERWRITE_VARS,
			  "isMultiJob", 0);
      mj=AH_Job_SingleTransfer_new(mu, ma);
      if (!mj) {
	DBG_ERROR(AQHBCI_LOGDOMAIN, "Job not supported with this account");
	return GWEN_ERROR_NOT_AVAILABLE;
      }
    }
    break;

  case AB_Job_TypeDebitNote:
    mj=0;
    if (!(aFlags & AH_BANK_FLAGS_PREFER_SINGLE_DEBITNOTE)) {
      DBG_INFO(AQHBCI_LOGDOMAIN, "Customer prefers multi jobs");
      /* create new job */
      mj=AH_Job_MultiDebitNote_new(mu, ma);
      if (!mj) {
	DBG_WARN(AQHBCI_LOGDOMAIN,
		 "Multi-job not supported with this account, "
		 "using single-job");
      }
    }
    else {
      DBG_INFO(AQHBCI_LOGDOMAIN, "Customer prefers single jobs");
    }
    if (mj) {
      GWEN_DB_SetIntValue(dbJob, GWEN_DB_FLAGS_OVERWRITE_VARS,
			  "isMultiJob", 1);
    }
    else {
      GWEN_DB_SetIntValue(dbJob, GWEN_DB_FLAGS_OVERWRITE_VARS,
			  "isMultiJob", 0);
      mj=AH_Job_SingleDebitNote_new(mu, ma);
      if (!mj) {
	DBG_ERROR(AQHBCI_LOGDOMAIN, "Job not supported with this account");
	return GWEN_ERROR_NOT_AVAILABLE;
      }
    }
    break;

  case AB_Job_TypeEuTransfer:
    mj=AH_Job_EuTransfer_new(mu, ma);
    if (!mj) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Job not supported with this account");
      return GWEN_ERROR_NOT_AVAILABLE;
    }
    break;

  case AB_Job_TypeInternalTransfer:
    mj=AH_Job_InternalTransfer_new(mu, ma);
    if (!mj) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Job not supported with this account");
      return GWEN_ERROR_NOT_AVAILABLE;
    }
    break;

  case AB_Job_TypeCreateStandingOrder:
    mj=AH_Job_CreateStandingOrder_new(mu, ma);
    if (!mj) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Job not supported with this account");
      return GWEN_ERROR_NOT_AVAILABLE;
    }
    break;

  case AB_Job_TypeModifyStandingOrder:
    mj=AH_Job_ModifyStandingOrder_new(mu, ma);
    if (!mj) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Job not supported with this account");
      return GWEN_ERROR_NOT_AVAILABLE;
    }
    break;

  case AB_Job_TypeDeleteStandingOrder:
    mj=AH_Job_DeleteStandingOrder_new(mu, ma);
    if (!mj) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Job not supported with this account");
      return GWEN_ERROR_NOT_AVAILABLE;
    }
    break;

  case AB_Job_TypeCreateDatedTransfer:
    mj=AH_Job_CreateDatedTransfer_new(mu, ma);
    if (!mj) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Job not supported with this account");
      return GWEN_ERROR_NOT_AVAILABLE;
    }
    break;

  case AB_Job_TypeModifyDatedTransfer:
    mj=AH_Job_ModifyDatedTransfer_new(mu, ma);
    if (!mj) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Job not supported with this account");
      return GWEN_ERROR_NOT_AVAILABLE;
    }
    break;

  case AB_Job_TypeDeleteDatedTransfer:
    mj=AH_Job_DeleteDatedTransfer_new(mu, ma);
    if (!mj) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Job not supported with this account");
      return GWEN_ERROR_NOT_AVAILABLE;
    }
    break;

  case AB_Job_TypeLoadCellPhone:
    mj=AH_Job_LoadCellPhone_new(mu, ma);
    if (!mj) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Job not supported with this account");
      return GWEN_ERROR_NOT_AVAILABLE;
    }
    break;

  default:
    return GWEN_ERROR_NOT_AVAILABLE;
  }

  /* exchange parameters */
  rv=AH_Job_Exchange(mj, j, AH_Job_ExchangeModeParams, NULL);
  if (rv) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Error exchanging params");
    AH_Job_free(mj);
    return rv;
  }

  /* free my job, it is no longer needed here */
  AH_Job_free(mj);

  DBG_INFO(AQHBCI_LOGDOMAIN, "Job successfully updated");
  return 0;
}



int AH_Provider_AddJob(AB_PROVIDER *pro, AB_JOB *j){
  AH_PROVIDER *hp;
  GWEN_DB_NODE *dbJob;
  AH_JOB *mj;
  uint32_t jid;
  AB_ACCOUNT *ma;
  AB_USER *mu;
  int rv;
  int sigs;
  int jobIsNew=1;
  AB_JOB_STATUS jst;
  uint32_t uFlags;
  uint32_t aFlags;

  assert(pro);
  hp=GWEN_INHERIT_GETDATA(AB_PROVIDER, AH_PROVIDER, pro);
  assert(hp);

  if (hp->outbox==0)
    hp->outbox=AH_Outbox_new(hp->hbci);
  assert(hp->outbox);

  dbJob=AB_Job_GetProviderData(j, pro);
  assert(dbJob);

  jst=AB_Job_GetStatus(j);
  if (jst==AB_Job_StatusPending) {
    DBG_INFO(AQHBCI_LOGDOMAIN,
             "Adding pending job for verification");
    AH_Outbox_AddPendingJob(hp->outbox, j);
    return 0;
  }

  jid=AB_Job_GetIdForProvider(j);
  if (jid) {
    DBG_ERROR(AQHBCI_LOGDOMAIN,
              "Jobs has already been sent to this backend, rejecting");
    return GWEN_ERROR_INVALID;
  }

  ma=AB_Job_GetAccount(j);
  assert(ma);

  /* determine customer to use */
  mu=AB_Account_GetFirstUser(ma);
  if (!mu) {
    DBG_ERROR(AQHBCI_LOGDOMAIN,
              "No customers noted for account \"%s/%s\"",
              AB_Account_GetBankCode(ma),
              AB_Account_GetAccountNumber(ma));
    return GWEN_ERROR_NOT_AVAILABLE;
  }

  uFlags=AH_User_GetFlags(mu);
  aFlags=AH_Account_GetFlags(ma);

  mj=0;
  switch(AB_Job_GetType(j)) {

  case AB_Job_TypeGetBalance:
    mj=AH_Job_GetBalance_new(mu, ma);
    if (!mj) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Job not supported with this account");
      return GWEN_ERROR_NOT_AVAILABLE;
    }
    break;

  case AB_Job_TypeGetTransactions:
    mj=AH_Job_GetTransactions_new(mu, ma);
    if (!mj) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Job not supported with this account");
      return GWEN_ERROR_NOT_AVAILABLE;
    }
    break;

  case AB_Job_TypeGetStandingOrders:
    mj=AH_Job_GetStandingOrders_new(mu, ma);
    if (!mj) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Job not supported with this account");
      return GWEN_ERROR_NOT_AVAILABLE;
    }
    break;

  case AB_Job_TypeGetDatedTransfers:
    mj=AH_Job_GetDatedTransfers_new(mu, ma);
    if (!mj) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Job not supported with this account");
      return GWEN_ERROR_NOT_AVAILABLE;
    }
    break;

  case AB_Job_TypeTransfer:
    mj=0;
    if (!(aFlags & AH_BANK_FLAGS_PREFER_SINGLE_TRANSFER)) {
      DBG_INFO(AQHBCI_LOGDOMAIN, "Customer prefers multi jobs");
      mj=AH_Outbox_FindTransferJob(hp->outbox, mu, ma, 1);
      if (mj) {
	/* simply add transfer to existing job */
        jobIsNew=0;
        DBG_DEBUG(AQHBCI_LOGDOMAIN, "No matching job found");
      }
      else {
	/* create new job */
	mj=AH_Job_MultiTransfer_new(mu, ma);
	if (!mj) {
	  DBG_WARN(AQHBCI_LOGDOMAIN,
		    "Multi-job not supported with this account, "
		    "using single-job");
	}
      }
    }
    if (!mj) {
      mj=AH_Job_SingleTransfer_new(mu, ma);
      if (!mj) {
	DBG_ERROR(AQHBCI_LOGDOMAIN, "Job not supported with this account");
	return GWEN_ERROR_NOT_AVAILABLE;
      }
    }
    break;

  case AB_Job_TypeDebitNote:
    mj=0;
    if (!(aFlags & AH_BANK_FLAGS_PREFER_SINGLE_DEBITNOTE)) {
      DBG_INFO(AQHBCI_LOGDOMAIN, "Customer prefers multi jobs");
      mj=AH_Outbox_FindTransferJob(hp->outbox, mu, ma, 0);
      if (mj) {
	/* simply add transfer to existing job */
        jobIsNew=0;
        DBG_DEBUG(AQHBCI_LOGDOMAIN, "No matching job found");
      }
      else {
	/* create new job */
	mj=AH_Job_MultiDebitNote_new(mu, ma);
	if (!mj) {
	  DBG_ERROR(AQHBCI_LOGDOMAIN,
		    "Multi-job not supported with this account, "
		    "using single-job");
	}
      }
    }
    if (!mj) {
      mj=AH_Job_SingleDebitNote_new(mu, ma);
      if (!mj) {
	DBG_ERROR(AQHBCI_LOGDOMAIN, "Job not supported with this account");
	return GWEN_ERROR_NOT_AVAILABLE;
      }
    }
    break;

  case AB_Job_TypeInternalTransfer:
    mj=AH_Job_InternalTransfer_new(mu, ma);
    if (!mj) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Job not supported with this account");
      return GWEN_ERROR_NOT_AVAILABLE;
    }
    break;

  case AB_Job_TypeEuTransfer:
    mj=AH_Job_EuTransfer_new(mu, ma);
    if (!mj) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Job not supported with this account");
      return GWEN_ERROR_NOT_AVAILABLE;
    }
    break;

  case AB_Job_TypeCreateStandingOrder:
    mj=AH_Job_CreateStandingOrder_new(mu, ma);
    if (!mj) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Job not supported with this account");
      return GWEN_ERROR_NOT_AVAILABLE;
    }
    break;

  case AB_Job_TypeModifyStandingOrder:
    mj=AH_Job_ModifyStandingOrder_new(mu, ma);
    if (!mj) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Job not supported with this account");
      return GWEN_ERROR_NOT_AVAILABLE;
    }
    break;

  case AB_Job_TypeDeleteStandingOrder:
    mj=AH_Job_DeleteStandingOrder_new(mu, ma);
    if (!mj) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Job not supported with this account");
      return GWEN_ERROR_NOT_AVAILABLE;
    }
    break;

  case AB_Job_TypeCreateDatedTransfer:
    mj=AH_Job_CreateDatedTransfer_new(mu, ma);
    if (!mj) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Job not supported with this account");
      return GWEN_ERROR_NOT_AVAILABLE;
    }
    break;

  case AB_Job_TypeModifyDatedTransfer:
    mj=AH_Job_ModifyDatedTransfer_new(mu, ma);
    if (!mj) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Job not supported with this account");
      return GWEN_ERROR_NOT_AVAILABLE;
    }
    break;

  case AB_Job_TypeDeleteDatedTransfer:
    mj=AH_Job_DeleteDatedTransfer_new(mu, ma);
    if (!mj) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Job not supported with this account");
      return GWEN_ERROR_NOT_AVAILABLE;
    }
    break;

  case AB_Job_TypeLoadCellPhone:
    mj=AH_Job_LoadCellPhone_new(mu, ma);
    if (!mj) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Job not supported with this account");
      return GWEN_ERROR_NOT_AVAILABLE;
    }
    break;

  default:
    DBG_ERROR(AQHBCI_LOGDOMAIN,
              "Job not supported by AqHBCI");
    return GWEN_ERROR_NOT_AVAILABLE;
  } /* switch */
  assert(mj);

  if (jobIsNew) {
    /* check whether we need to sign the job */
    sigs=AH_Job_GetMinSignatures(mj);
    if (sigs) {
      if (sigs>1) {
	DBG_ERROR(AQHBCI_LOGDOMAIN, "Multiple signatures not yet supported");
	GWEN_Gui_ProgressLog(0,
			     GWEN_LoggerLevel_Error,
			     I18N("ERROR: Multiple signatures not "
				  "yet supported"));
	return GWEN_ERROR_GENERIC;
      }
      AH_Job_AddSigner(mj, AB_User_GetUserId(mu));
    }
  }

  /* store HBCI job, link both jobs */
  if (AH_Job_GetId(mj)==0) {
    jid=AB_Job_GetJobId(j);
    assert(jid);
    /* we now use the same id here */
    AH_Job_SetId(mj, jid);
  }
  AB_Job_SetIdForProvider(j, AH_Job_GetId(mj));

  /* exchange arguments */
  rv=AH_Job_Exchange(mj, j, AH_Job_ExchangeModeArgs, NULL);
  if (rv) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Error exchanging params");
    AH_Job_free(mj);
    return rv;
  }

  if (jobIsNew) {
    /* prevent outbox from freeing this job */
    AH_Job_Attach(mj);
    /* add job to outbox */
    AH_Outbox_AddJob(hp->outbox, mj);
  }
  AB_Job_Attach(j);
  AB_Job_List2_PushBack(hp->bankingJobs, j);
  AB_Job_SetStatus(j, AB_Job_StatusSent);

  DBG_INFO(AQHBCI_LOGDOMAIN, "Job successfully added");
  return 0;
}



AH_JOB *AH_Provider__FindMyJob(AH_JOB_LIST *mjl, uint32_t jid){
  AH_JOB *mj;

  assert(mjl);

  DBG_INFO(AQHBCI_LOGDOMAIN, "Looking for id %08x", jid);
  mj=AH_Job_List_First(mjl);
  while(mj) {
    DBG_INFO(AQHBCI_LOGDOMAIN, "Comparing %08x", AH_Job_GetId(mj));
    if (AH_Job_GetId(mj)==jid)
      break;
    mj=AH_Job_List_Next(mj);
  }

  return mj;
}



int AH_Provider_Execute(AB_PROVIDER *pro, AB_IMEXPORTER_CONTEXT *ctx){
  AH_PROVIDER *hp;
  int rv;
  AB_JOB_LIST2_ITERATOR *jit;
  int successfull;
  AH_JOB_LIST *mjl;

  assert(pro);
  hp=GWEN_INHERIT_GETDATA(AB_PROVIDER, AH_PROVIDER, pro);
  assert(hp);

  successfull=0;
  if (hp->outbox==0) {
    DBG_WARN(AQHBCI_LOGDOMAIN, "Empty outbox");
    return 0;
  }

  rv=AH_Outbox_Execute(hp->outbox, ctx, 0, 1, 1);
  if (rv) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Error executing outbox.");
    rv=GWEN_ERROR_GENERIC;
  }

  /* fill gaps */
  AB_Banking_FillGapsInImExporterContext(AB_Provider_GetBanking(pro), ctx);

  mjl=AH_Outbox_GetFinishedJobs(hp->outbox);
  assert(mjl);
  /* copy job results to Banking-job, set status etc */
  jit=AB_Job_List2_First(hp->bankingJobs);
  if (jit) {
    AB_JOB *bj;

    bj=AB_Job_List2Iterator_Data(jit);
    assert(bj);
    while(bj) {
      AH_JOB *mj;
      GWEN_DB_NODE *beData;
      const char *s;
      const GWEN_STRINGLIST *sl;
      GWEN_STRINGLISTENTRY *se;
      AB_MESSAGE_LIST *ml;

      mj=AH_Provider__FindMyJob(mjl, AB_Job_GetIdForProvider(bj));
      assert(mj);

      beData=AB_Job_GetProviderData(bj, pro);
      assert(beData);

      /* store used TAN (if any) */
      s=AH_Job_GetUsedTan(mj);
      if (s) {
        GWEN_DB_SetCharValue(beData, GWEN_DB_FLAGS_OVERWRITE_VARS,
                             "usedTan", s);
        AB_Job_SetUsedTan(bj, s);
      }

      if (getenv("AQHBCI_DEBUG_JOBS")) { /* DEBUG */
        GWEN_DB_NODE *dbDebug;

        dbDebug=GWEN_DB_GetGroup(beData, GWEN_DB_FLAGS_OVERWRITE_GROUPS,
                                 "rawParams");
        assert(dbDebug);
        GWEN_DB_AddGroupChildren(dbDebug,
                                 AH_Job_GetParams(mj));
        dbDebug=GWEN_DB_GetGroup(beData, GWEN_DB_FLAGS_OVERWRITE_GROUPS,
                                    "rawArgs");
        assert(dbDebug);
        GWEN_DB_AddGroupChildren(dbDebug,
                                 AH_Job_GetArguments(mj));

        dbDebug=GWEN_DB_GetGroup(beData, GWEN_DB_FLAGS_OVERWRITE_GROUPS,
                                 "rawResponses");
        assert(dbDebug);
        GWEN_DB_AddGroupChildren(dbDebug,
                                 AH_Job_GetResponses(mj));
      }

      /* exchange logs */
      sl=AH_Job_GetLogs(mj);
      assert(sl);
      se=GWEN_StringList_FirstEntry(sl);
      while(se) {
        const char *s;

        s=GWEN_StringListEntry_Data(se);
        assert(s);
        AB_Job_LogRaw(bj, s);
        se=GWEN_StringListEntry_Next(se);
      }

      /* copy messages from AH_JOB to imexporter context */
      ml=AH_Job_GetMessages(mj);
      if (ml) {
	AB_MESSAGE *msg;

	msg=AB_Message_List_First(ml);
	while(msg) {
	  AB_ImExporterContext_AddMessage(ctx, AB_Message_dup(msg));
	  msg=AB_Message_List_Next(msg);
	}
      }

      /* exchange results */
      rv=AH_Job_Exchange(mj, bj, AH_Job_ExchangeModeResults, ctx);
      if (rv) {
        DBG_ERROR(AQHBCI_LOGDOMAIN, "Error exchanging results");
        AB_Job_SetStatus(bj, AB_Job_StatusError);
        AB_Job_SetResultText(bj, "Could not exchange results");
      }
      else {
        /* exchange was ok */
        if (AH_Job_HasErrors(mj)) {
          if (AB_Job_GetStatus(bj)==AB_Job_StatusSent) {
            AB_Job_SetStatus(bj, AB_Job_StatusError);
            /* TODO: Copy errors */
            AB_Job_SetResultText(bj, "Job contains errors");
	    AB_Job_Log(bj, GWEN_LoggerLevel_Warning, "aqhbci",
                       "Job contains errors");
          }
        }
        else {
          /* job is ok */
          if (AB_Job_GetStatus(bj)==AB_Job_StatusSent) {
            AB_Job_SetStatus(bj, AB_Job_StatusFinished);
            AB_Job_Log(bj, GWEN_LoggerLevel_Notice, "aqhbci",
                       "Job finished successfully");
            AB_Job_SetResultText(bj, "Ok.");
          }
          successfull++;
        }
      }
      bj=AB_Job_List2Iterator_Next(jit);
    } /* while */
    AB_Job_List2Iterator_free(jit);
  }

  /* free outbox, the next AddJob call will create a new one */
  AH_Outbox_free(hp->outbox);
  hp->outbox=0;

  /* release all jobs from my hold. If the application still has a hold
   * on a job then the following "free" will not actually free
   * that job but decrement its usage counter. */
  AB_Job_List2_FreeAll(hp->bankingJobs);
  hp->bankingJobs=AB_Job_List2_new();

  if (!successfull)
    return GWEN_ERROR_GENERIC;

  return 0;
}



int AH_Provider_ResetQueue(AB_PROVIDER *pro){
  AH_PROVIDER *hp;
  AH_HBCI *h;

  assert(pro);
  hp=GWEN_INHERIT_GETDATA(AB_PROVIDER, AH_PROVIDER, pro);
  assert(hp);

  h=AH_Provider_GetHbci(pro);
  assert(h);

  /* free outbox, the next AddJob call will create a new one */
  AH_Outbox_free(hp->outbox);
  hp->outbox=0;

  /* release all jobs from my hold. If the application still has a hold
   * on a job then the following "free" will not actually free
   * that job but decrement its usage counter. */
  AB_Job_List2_FreeAll(hp->bankingJobs);
  hp->bankingJobs=AB_Job_List2_new();

  return 0;
}



int AH_Provider_ExtendUser(AB_PROVIDER *pro, AB_USER *u,
			   AB_PROVIDER_EXTEND_MODE em,
			   GWEN_DB_NODE *db) {
  AH_User_Extend(u, pro, em, db);
  return 0;
}



int AH_Provider_ExtendAccount(AB_PROVIDER *pro, AB_ACCOUNT *a,
			      AB_PROVIDER_EXTEND_MODE em,
			      GWEN_DB_NODE *db){
  AH_Account_Extend(a, pro, em, db);
  return 0;
}



int AH_Provider_Update(AB_PROVIDER *pro,
                       uint32_t lastVersion,
		       uint32_t currentVersion) {
  if (lastVersion<((1<<24) | (8<<16) | (1<<8) | 3)) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Version is too old, can't autoupgrade");

    GWEN_Gui_MessageBox(GWEN_GUI_MSG_FLAGS_TYPE_INFO |
			GWEN_GUI_MSG_FLAGS_CONFIRM_B1 |
			GWEN_GUI_MSG_FLAGS_SEVERITY_NORMAL,
			I18N("AqHBCI-Notice"),
			I18N(
    "The version of AqBanking/AqHBCI previously used is too old to be\n"
    "upgraded automatically.\n"
    "Therefore you should delete the settings file and setup AqBanking\n"
    "completely from scratch.\n"
    "The settings file usually is\n"
     "  $HOME/.banking/settings.conf\n"
     "<html>"
    "<p>"
    "The version of AqBanking/AqHBCI previously used is too old to be\n"
    "upgraded automatically.\n"
    "</p>"
    "<p>"
    "Therefore you should delete the settings file and setup AqBanking\n"
    "completely from scratch.\n"
    "</p>"
    "<p>"
    "The settings file usually is: \n"
    "<i>"
    "$HOME/.banking/settings.conf\n"
    "</i>.\n"
     "</p>"
     "</html>"
			    ),
			I18N("Continue"), 0, 0, 0);
    return GWEN_ERROR_INTERNAL;
  }

  return 0;
}



GWEN_DIALOG *AH_Provider_GetNewCardUserDialog(AB_PROVIDER *pro) {
  int rv;
  GWEN_BUFFER *mtypeName;
  GWEN_BUFFER *mediumName;
  GWEN_CRYPT_TOKEN *ct;

  mtypeName=GWEN_Buffer_new(0, 64, 0, 1);
  mediumName=GWEN_Buffer_new(0, 64, 0, 1);

  rv=AB_Banking_CheckCryptToken(AB_Provider_GetBanking(pro),
				GWEN_Crypt_Token_Device_Card,
				mtypeName,
				mediumName);
  if (rv<0) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "here (%d)", rv);
    GWEN_Buffer_free(mediumName);
    GWEN_Buffer_free(mtypeName);
    return NULL;
  }

  rv=AB_Banking_GetCryptToken(AB_Provider_GetBanking(pro),
			      GWEN_Buffer_GetStart(mtypeName),
			      GWEN_Buffer_GetStart(mediumName),
			      &ct);
  if (rv<0) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "here (%d)", rv);
    GWEN_Buffer_free(mediumName);
    GWEN_Buffer_free(mtypeName);
    return NULL;
  }

  if (strcasecmp(GWEN_Buffer_GetStart(mtypeName), "ddvcard")==0) {
    GWEN_DIALOG *dlg2;

    DBG_ERROR(0, "DDV card");
    dlg2=AH_DdvCardDialog_new(AB_Provider_GetBanking(pro), ct);
    if (dlg2==NULL) {
      DBG_INFO(AQHBCI_LOGDOMAIN, "here (no dialog)");
      GWEN_Buffer_free(mediumName);
      GWEN_Buffer_free(mtypeName);
      return NULL;
    }

    GWEN_Dialog_SetWidgetText(dlg2, "", I18N("Create HBCI/FinTS DDV User"));
    GWEN_Buffer_free(mediumName);
    GWEN_Buffer_free(mtypeName);
    return dlg2;
  }
  else if (strcasecmp(GWEN_Buffer_GetStart(mtypeName), "starcoscard")==0) {
    DBG_ERROR(0, "STARCOS RSA card");
    // TODO
  }
  else {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Card type \"%s\" not yet supported",
	      GWEN_Buffer_GetStart(mtypeName));
  }
  GWEN_Buffer_free(mediumName);
  GWEN_Buffer_free(mtypeName);
  AB_Banking_ClearCryptTokenList(AB_Provider_GetBanking(pro));

  return NULL;
}



GWEN_DIALOG *AH_Provider_GetEditUserDialog(AB_PROVIDER *pro, AB_USER *u) {
  AH_PROVIDER *hp;
  GWEN_DIALOG *dlg;

  assert(pro);
  hp=GWEN_INHERIT_GETDATA(AB_PROVIDER, AH_PROVIDER, pro);
  assert(hp);

  switch(AH_User_GetCryptMode(u)) {
  case AH_CryptMode_Pintan:
    dlg=AH_EditUserPinTanDialog_new(AB_Provider_GetBanking(pro), u, 1);
    break;
  case AH_CryptMode_Ddv:
    dlg=AH_EditUserDdvDialog_new(AB_Provider_GetBanking(pro), u, 1);
    break;
  case AH_CryptMode_Rdh:
    dlg=AH_EditUserRdhDialog_new(AB_Provider_GetBanking(pro), u, 1);
    break;
  default:
    dlg=NULL;
    break;
  }

  if (dlg==NULL) {
    DBG_INFO(AQHBCI_LOGDOMAIN, "here (no dialog)");
    return NULL;
  }

  return dlg;
}



GWEN_DIALOG *AH_Provider_GetNewUserDialog(AB_PROVIDER *pro, int i) {
  AH_PROVIDER *hp;
  GWEN_DIALOG *dlg;

  assert(pro);
  hp=GWEN_INHERIT_GETDATA(AB_PROVIDER, AH_PROVIDER, pro);
  assert(hp);

  switch(i) {
  case AqHBCI_NewUserDialog_CodeExistingPinTan:
    dlg=AH_PinTanDialog_new(AB_Provider_GetBanking(pro));
    break;

  case AqHBCI_NewUserDialog_CodeExistingChipcard:
    dlg=AH_Provider_GetNewCardUserDialog(pro);
    break;

  case AqHBCI_NewUserDialog_CodeCreateKeyFile:
    dlg=AH_NewKeyFileDialog_new(AB_Provider_GetBanking(pro));
    break;

  case AqHBCI_NewUserDialog_CodeExistingKeyFile:
    dlg=AH_ImportKeyFileDialog_new(AB_Provider_GetBanking(pro));
    break;
  case AqHBCI_NewUserDialog_CodeCreateChipcard:

  case AqHBCI_NewUserDialog_CodeGeneric:
  default:
    dlg=AH_NewUserDialog_new(AB_Provider_GetBanking(pro));
    break;
  }

  if (dlg==NULL) {
    DBG_INFO(AQHBCI_LOGDOMAIN, "here (no dialog)");
    return NULL;
  }

  return dlg;
}



GWEN_DIALOG *AH_Provider_GetUserTypeDialog(AB_PROVIDER *pro) {
  AH_PROVIDER *hp;
  GWEN_DIALOG *dlg;

  assert(pro);
  hp=GWEN_INHERIT_GETDATA(AB_PROVIDER, AH_PROVIDER, pro);
  assert(hp);

  dlg=AH_ChooseUserTypeDialog_new(AB_Provider_GetBanking(pro));
  if (dlg==NULL) {
    DBG_INFO(AQHBCI_LOGDOMAIN, "here (no dialog)");
    return NULL;
  }

  return dlg;
}



AH_HBCI *AH_Provider_GetHbci(const AB_PROVIDER *pro){
  AH_PROVIDER *hp;

  assert(pro);
  hp=GWEN_INHERIT_GETDATA(AB_PROVIDER, AH_PROVIDER, pro);
  assert(hp);

  return hp->hbci;
}



int AH_Provider_GetAccounts(AB_PROVIDER *pro, AB_USER *u,
                            AB_IMEXPORTER_CONTEXT *ctx,
			    int withProgress, int nounmount, int doLock) {
  AB_BANKING *ab;
  AH_HBCI *h;
  AH_JOB *job;
  AH_OUTBOX *ob;
  AB_ACCOUNT_LIST2 *accs;
  int rv;
  AH_PROVIDER *hp;

  assert(pro);
  hp=GWEN_INHERIT_GETDATA(AB_PROVIDER, AH_PROVIDER, pro);
  assert(hp);

  assert(u);

  ab=AB_Provider_GetBanking(pro);
  assert(ab);

  h=AH_Provider_GetHbci(pro);
  assert(h);

  job=AH_Job_UpdateBank_new(u);
  if (!job) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Job not supported, should not happen");
    return GWEN_ERROR_GENERIC;
  }
  AH_Job_AddSigner(job, AB_User_GetUserId(u));

  ob=AH_Outbox_new(h);
  AH_Outbox_AddJob(ob, job);

  rv=AH_Outbox_Execute(ob, ctx, withProgress, 1, doLock);
  if (rv) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Could not execute outbox.\n");
    if (!nounmount)
      AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
    return rv;
  }

  if (AH_Job_HasErrors(job)) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Job has errors");
    // TODO: show errors
    AH_Outbox_free(ob);
    if (!nounmount)
      AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
    return GWEN_ERROR_GENERIC;
  }
  else {
    rv=AH_Job_Commit(job, doLock);
    if (rv) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Could not commit result.\n");
      AH_Outbox_free(ob);
      if (!nounmount)
	AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
      return rv;
    }
  }

  /* check whether we got some accounts */
  accs=AH_Job_UpdateBank_GetAccountList(job);
  assert(accs);
  if (AB_Account_List2_GetSize(accs)==0) {
    DBG_INFO(AQHBCI_LOGDOMAIN, "No accounts found");
    if (!nounmount)
      AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
    return GWEN_ERROR_NO_DATA;
  }

  AH_Outbox_free(ob);
  if (!nounmount)
    AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
  return 0;
}



int AH_Provider_GetSysId(AB_PROVIDER *pro, AB_USER *u,
                         AB_IMEXPORTER_CONTEXT *ctx,
			 int withProgress, int nounmount, int doLock) {
  AB_BANKING *ab;
  AH_HBCI *h;
  AH_JOB *job;
  AH_OUTBOX *ob;
  int rv;
  AH_PROVIDER *hp;
  const char *s;
  int i;
  char tbuf[256];

  assert(pro);
  hp=GWEN_INHERIT_GETDATA(AB_PROVIDER, AH_PROVIDER, pro);
  assert(hp);

  assert(u);

  ab=AB_Provider_GetBanking(pro);
  assert(ab);

  h=AH_Provider_GetHbci(pro);
  assert(h);

  job=0;
  ob=0;
  rv=0;
  for (i=0; ; i++) {
    job=AH_Job_GetSysId_new(u);
    if (!job) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Job not supported, should not happen");
      AB_Banking_EndExclUseUser(ab, u, 1);
      return GWEN_ERROR_GENERIC;
    }
    AH_Job_AddSigner(job, AB_User_GetUserId(u));

    ob=AH_Outbox_new(h);
    AH_Outbox_AddJob(ob, job);

    rv=AH_Outbox_Execute(ob, ctx, withProgress, 1, doLock);
    if (rv) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Could not execute outbox.\n");
      if (!nounmount)
	AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
      return rv;
    }

    if (AH_Job_HasErrors(job)) {
      if (AH_Job_HasItanResult(job)) {
	GWEN_Gui_ProgressLog(0,
			     GWEN_LoggerLevel_Error,
			     I18N("Adjusting to iTAN modes of the server"));
	rv=AH_Job_Commit(job, doLock);
        if (rv) {
          DBG_ERROR(AQHBCI_LOGDOMAIN, "Could not commit result.\n");
          AH_Outbox_free(ob);
          if (!nounmount)
	    AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
          return rv;
	}

#if 0
	/* save user in order to get the info written to config database for
         * inspection while debugging
         */
        rv=AB_Banking_SaveUser(ab, u);
	if (rv<0) {
	  DBG_ERROR(AQHBCI_LOGDOMAIN, "Error saving user (%d)", rv);
	  AH_Outbox_free(ob);
	  if (!nounmount)
	    AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
	  return rv;
	}
#endif

	rv=GWEN_Gui_ProgressLog(0,
				GWEN_LoggerLevel_Error,
				I18N("Retrying to get system id."));
	if (rv) {
	  DBG_ERROR(AQHBCI_LOGDOMAIN, "Error in progress log, maybe user aborted?");
	  AH_Outbox_free(ob);
	  if (!nounmount)
	    AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
	  return rv;
	}
      }
      else {
        DBG_ERROR(AQHBCI_LOGDOMAIN, "Job has errors");
        // TODO: show errors
        AH_Outbox_free(ob);
        if (!nounmount)
	  AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
        return GWEN_ERROR_GENERIC;
      }
    }
    else {
      rv=AH_Job_Commit(job, doLock);
      if (rv) {
        DBG_ERROR(AQHBCI_LOGDOMAIN, "Could not commit result.\n");
        AH_Outbox_free(ob);
        if (!nounmount)
	  AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
        return rv;
      }
      break;
    }
    AH_Job_free(job);
    AH_Outbox_free(ob);
    if (i>1) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Tried too often, giving up");
      GWEN_Gui_ProgressLog(0,
			   GWEN_LoggerLevel_Error,
			   I18N("Could not get system id after multiple trials"));
      if (!nounmount)
	AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
      return GWEN_ERROR_GENERIC;
    }
  }

  /* lock user */
  if (doLock) {
    rv=AB_Banking_BeginExclUseUser(ab, u);
    if (rv) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Could not lock user (%d)\n", rv);
      AH_Outbox_free(ob);
      if (!nounmount)
	AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
      return rv;
    }
  }

  s=AH_Job_GetSysId_GetSysId(job);
  if (!s) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "No system id");
    if (doLock)
      AB_Banking_EndExclUseUser(ab, u, 1);
    AH_Outbox_free(ob);
    if (!nounmount)
      AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
    return GWEN_ERROR_NO_DATA;
  }

  AH_User_SetSystemId(u, s);

  /* unlock user */
  if (doLock) {
    rv=AB_Banking_EndExclUseUser(ab, u, 0);
    if (rv<0) {
      DBG_INFO(AQHBCI_LOGDOMAIN,
	       "Could not unlock customer [%s] (%d)",
	       AB_User_GetCustomerId(u), rv);
      snprintf(tbuf, sizeof(tbuf)-1,
	       I18N("Could not unlock user %s (%d)"),
	       AB_User_GetUserId(u), rv);
      tbuf[sizeof(tbuf)-1]=0;
      GWEN_Gui_ProgressLog(0,
			   GWEN_LoggerLevel_Error,
			   tbuf);
      AB_Banking_EndExclUseUser(ab, u, 1);
      AH_Outbox_free(ob);
      if (!nounmount)
	AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
      return rv;
    }
  }

  AH_Outbox_free(ob);

  if (!nounmount)
    AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));

  return 0;
}



int AH_Provider_GetServerKeys(AB_PROVIDER *pro, AB_USER *u,
                              AB_IMEXPORTER_CONTEXT *ctx,
			      int withProgress, int nounmount, int doLock) {
  AB_BANKING *ab;
  AH_HBCI *h;
  AH_JOB *job;
  AH_OUTBOX *ob;
  int rv;
  AH_PROVIDER *hp;
  GWEN_CRYPT_TOKEN *ct;
  const GWEN_CRYPT_TOKEN_CONTEXT *cctx;
  const char *s;

  assert(pro);
  hp=GWEN_INHERIT_GETDATA(AB_PROVIDER, AH_PROVIDER, pro);
  assert(hp);

  assert(u);

  ab=AB_Provider_GetBanking(pro);
  assert(ab);

  h=AH_Provider_GetHbci(pro);
  assert(h);

  job=AH_Job_GetKeys_new(u);
  if (!job) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Job not supported, should not happen");
    return GWEN_ERROR_GENERIC;
  }

  ob=AH_Outbox_new(h);
  AH_Outbox_AddJob(ob, job);

  rv=AH_Outbox_Execute(ob, ctx, withProgress, 1, doLock);
  if (rv) {
    GWEN_Gui_ProgressLog(0,
			 GWEN_LoggerLevel_Error,
			 I18N("Could not execute outbox."));
    if (!nounmount)
      AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
    return rv;
  }

  if (AH_Job_GetKeys_GetCryptKeyInfo(job)==NULL) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "No crypt key received");
    GWEN_Gui_ProgressLog(0,
			 GWEN_LoggerLevel_Error,
			 I18N("No crypt key received."));
    AH_Outbox_free(ob);
    if (!nounmount)
      AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
    return GWEN_ERROR_GENERIC;
  }
  else {
    rv=AH_Job_Commit(job, doLock);
    if (rv) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Could not commit result.\n");
      GWEN_Gui_ProgressLog(0,
			   GWEN_LoggerLevel_Error,
			   I18N("Could not commit result"));
      AH_Outbox_free(ob);
      if (!nounmount)
	AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
      return rv;
    }
  }

  if (AH_Job_GetKeys_GetSignKeyInfo(job)==0) {
    GWEN_Gui_ProgressLog(0,
			 GWEN_LoggerLevel_Notice,
			 I18N("Bank does not use a sign key."));
  }

  /* lock user */
  if (doLock) {
    rv=AB_Banking_BeginExclUseUser(ab, u);
    if (rv) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Could not lock user (%d)\n", rv);
      AH_Outbox_free(ob);
      if (!nounmount)
	AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
      return rv;
    }
  }

  s=AH_User_GetPeerId(u);
  if (!s || !*s) {
    s=AH_Job_GetKeys_GetPeerId(job);
    if (s && *s) {
      char tbuf[256];

      snprintf(tbuf, sizeof(tbuf)-1, I18N("Setting peer ID to \"%s\")"), s);
      tbuf[sizeof(tbuf)-1]=0;
      GWEN_Gui_ProgressLog(0,
			   GWEN_LoggerLevel_Notice,
			   tbuf);
      AH_User_SetPeerId(u, s);
    }
  }

  /* get crypt token */
  rv=AB_Banking_GetCryptToken(AH_HBCI_GetBankingApi(h),
			      AH_User_GetTokenType(u),
			      AH_User_GetTokenName(u),
			      &ct);
  if (rv) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Could not get crypt token (%d)", rv);
    GWEN_Gui_ProgressLog(0,
			 GWEN_LoggerLevel_Error,
			 I18N("Error getting crypt token"));
    if (doLock)
      AB_Banking_EndExclUseUser(ab, u, 0);
    AH_Outbox_free(ob);
    if (!nounmount)
      AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
    return rv;
  }

  /* open crypt token */
  rv=GWEN_Crypt_Token_Open(ct, 1, 0);
  if (rv) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Could not open crypt token (%d)", rv);
    GWEN_Gui_ProgressLog(0,
			 GWEN_LoggerLevel_Error,
			 I18N("Error opening crypt token"));
    if (doLock)
      AB_Banking_EndExclUseUser(ab, u, 0);
    AH_Outbox_free(ob);
    if (!nounmount)
      AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
    return rv;
  }

  /* get context */
  cctx=GWEN_Crypt_Token_GetContext(ct, AH_User_GetTokenContextId(u), 0);
  if (!cctx) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "User context not found on crypt token");
    GWEN_Gui_ProgressLog(0,
			 GWEN_LoggerLevel_Error,
			 I18N("User context not found on crypt token"));
    if (doLock)
      AB_Banking_EndExclUseUser(ab, u, 0);
    AH_Outbox_free(ob);
    if (!nounmount)
      AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
    return GWEN_ERROR_NOT_FOUND;
  }
  else {
    GWEN_CRYPT_TOKEN_KEYINFO *ki;
    uint32_t kid;

    /* store sign key (if any) */
    kid=GWEN_Crypt_Token_Context_GetVerifyKeyId(cctx);
    ki=AH_Job_GetKeys_GetSignKeyInfo(job);
    if (kid && ki) {
      rv=GWEN_Crypt_Token_SetKeyInfo(ct,
				     kid,
				     ki,
				     0);
      if (rv) {
	DBG_ERROR(AQHBCI_LOGDOMAIN, "Could not save key info (%d)", rv);
	GWEN_Gui_ProgressLog(0,
			     GWEN_LoggerLevel_Error,
			     I18N("Error saving sign key"));
	if (doLock)
	  AB_Banking_EndExclUseUser(ab, u, 0);
	AH_Outbox_free(ob);
	if (!nounmount)
	  AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
	return rv;
      }
      DBG_INFO(AQHBCI_LOGDOMAIN, "Sign key saved");
    }

    /* store crypt key */
    kid=GWEN_Crypt_Token_Context_GetEncipherKeyId(cctx);
    ki=AH_Job_GetKeys_GetCryptKeyInfo(job);
    if (kid && ki) {
      rv=GWEN_Crypt_Token_SetKeyInfo(ct,
                                     kid,
				     ki,
				     0);
      if (rv) {
	DBG_ERROR(AQHBCI_LOGDOMAIN, "Could not save key info (%d)", rv);
	GWEN_Gui_ProgressLog(0,
			     GWEN_LoggerLevel_Error,
			     I18N("Error saving crypt key"));
	if (doLock)
	  AB_Banking_EndExclUseUser(ab, u, 0);
	AH_Outbox_free(ob);
	if (!nounmount)
	  AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
	return rv;
      }
      DBG_INFO(AQHBCI_LOGDOMAIN, "Crypt key saved");
    }

    /* store auth key (if any) */
    kid=GWEN_Crypt_Token_Context_GetAuthVerifyKeyId(cctx);
    ki=AH_Job_GetKeys_GetAuthKeyInfo(job);
    if (kid && ki) {
      rv=GWEN_Crypt_Token_SetKeyInfo(ct,
				     kid,
				     ki,
				     0);
      if (rv) {
	DBG_ERROR(AQHBCI_LOGDOMAIN, "Could not save key info (%d)", rv);
	GWEN_Gui_ProgressLog(0,
			     GWEN_LoggerLevel_Error,
			     I18N("Error saving auth key"));
	if (doLock)
	  AB_Banking_EndExclUseUser(ab, u, 0);
	AH_Outbox_free(ob);
	if (!nounmount)
	  AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
	return rv;
      }
      DBG_INFO(AQHBCI_LOGDOMAIN, "Auth key saved");
    }
  }

  GWEN_Gui_ProgressLog(0,
		       GWEN_LoggerLevel_Notice,
		       I18N("Keys saved."));

  /* unlock user */
  if (doLock) {
    rv=AB_Banking_EndExclUseUser(ab, u, 0);
    if (rv) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Could not unlock user (%d)\n", rv);
      AH_Outbox_free(ob);
      if (!nounmount)
	AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
      return rv;
    }
  }

  AH_Outbox_free(ob);
  if (!nounmount)
    AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));

  return 0;
}



int AH_Provider_SendUserKeys2(AB_PROVIDER *pro, AB_USER *u,
			      AB_IMEXPORTER_CONTEXT *ctx,
			      int withAuthKey,
			      int withProgress, int nounmount, int doLock) {
  AB_BANKING *ab;
  AH_HBCI *h;
  AH_JOB *job;
  AH_OUTBOX *ob;
  int rv;
  AH_PROVIDER *hp;
  GWEN_CRYPT_TOKEN *ct;
  uint32_t kid;
  const GWEN_CRYPT_TOKEN_CONTEXT *cctx;
  const GWEN_CRYPT_TOKEN_KEYINFO *signKeyInfo=NULL;
  const GWEN_CRYPT_TOKEN_KEYINFO *cryptKeyInfo=NULL;
  const GWEN_CRYPT_TOKEN_KEYINFO *authKeyInfo=NULL;
  int mounted=0;

  assert(pro);
  hp=GWEN_INHERIT_GETDATA(AB_PROVIDER, AH_PROVIDER, pro);
  assert(hp);

  assert(u);

  ab=AB_Provider_GetBanking(pro);
  assert(ab);

  h=AH_Provider_GetHbci(pro);
  assert(h);

  /* get crypt token */
  rv=AB_Banking_GetCryptToken(AH_HBCI_GetBankingApi(h),
			      AH_User_GetTokenType(u),
			      AH_User_GetTokenName(u),
			      &ct);
  if (rv) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Could not get crypt token (%d)", rv);
    GWEN_Gui_ProgressLog(0,
			 GWEN_LoggerLevel_Error,
			 I18N("Error getting crypt token"));
    if (!nounmount)
      AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
    return rv;
  }

  /* open crypt token */
  rv=GWEN_Crypt_Token_Open(ct, 1, 0);
  if (rv) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Could not open crypt token (%d)", rv);
    GWEN_Gui_ProgressLog(0,
			 GWEN_LoggerLevel_Error,
			 I18N("Error opening crypt token"));
    if (!nounmount)
      AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
    return rv;
  }

  /* get context */
  cctx=GWEN_Crypt_Token_GetContext(ct, AH_User_GetTokenContextId(u), 0);
  if (!cctx) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "User context not found on crypt token");
    GWEN_Gui_ProgressLog(0,
			 GWEN_LoggerLevel_Error,
			 I18N("User context not found on crypt token"));
    if (!nounmount)
      AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
    return GWEN_ERROR_NOT_FOUND;
  }

  /* get sign key info */
  kid=GWEN_Crypt_Token_Context_GetSignKeyId(cctx);
  if (kid) {
    signKeyInfo=GWEN_Crypt_Token_GetKeyInfo(ct, kid,
					    GWEN_CRYPT_TOKEN_KEYFLAGS_HASMODULUS |
					    GWEN_CRYPT_TOKEN_KEYFLAGS_HASEXPONENT |
					    GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYVERSION |
                                            GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYNUMBER,
					    0);
    if (signKeyInfo==NULL) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Sign key info not found on crypt token");
      GWEN_Gui_ProgressLog(0,
			   GWEN_LoggerLevel_Error,
			   I18N("Sign key info not found on crypt token"));
      if (!nounmount)
	AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
      return GWEN_ERROR_NOT_FOUND;
    }
  }
  else {
    DBG_INFO(AQHBCI_LOGDOMAIN, "No sign key id");
  }

  /* get crypt key info */
  kid=GWEN_Crypt_Token_Context_GetDecipherKeyId(cctx);
  if (kid) {
    cryptKeyInfo=GWEN_Crypt_Token_GetKeyInfo(ct, kid,
					     GWEN_CRYPT_TOKEN_KEYFLAGS_HASMODULUS |
					     GWEN_CRYPT_TOKEN_KEYFLAGS_HASEXPONENT |
					     GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYVERSION |
					     GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYNUMBER,
					     0);
    if (cryptKeyInfo==NULL) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Crypt key info not found on crypt token");
      GWEN_Gui_ProgressLog(0,
			   GWEN_LoggerLevel_Error,
			   I18N("Crypt key info not found on crypt token"));
      if (!nounmount)
	AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
      return GWEN_ERROR_NOT_FOUND;
    }
  }
  else {
    DBG_INFO(AQHBCI_LOGDOMAIN, "No decipher key id");
  }

  /* get auth sign key info */
  if (withAuthKey) {
    kid=GWEN_Crypt_Token_Context_GetAuthSignKeyId(cctx);
    if (kid) {
      authKeyInfo=GWEN_Crypt_Token_GetKeyInfo(ct, kid,
					      GWEN_CRYPT_TOKEN_KEYFLAGS_HASMODULUS |
					      GWEN_CRYPT_TOKEN_KEYFLAGS_HASEXPONENT |
					      GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYVERSION |
					      GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYNUMBER,
					      0);
      if (authKeyInfo==NULL) {
	DBG_ERROR(AQHBCI_LOGDOMAIN, "Auth key info not found on crypt token");
	GWEN_Gui_ProgressLog(0,
			     GWEN_LoggerLevel_Error,
			     I18N("Auth key info not found on crypt token"));
	if (!nounmount)
	  AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
	return GWEN_ERROR_NOT_FOUND;
      }
    }
    else {
      DBG_INFO(AQHBCI_LOGDOMAIN, "No auth key id");
    }
  }

  /* create job */
  job=AH_Job_SendKeys_new(u, cryptKeyInfo, signKeyInfo, authKeyInfo);
  if (!job) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Job not supported, should not happen");
    GWEN_Gui_ProgressLog(0,
			 GWEN_LoggerLevel_Error,
			 I18N("Job not supported, should not happen"));
    if (!nounmount && mounted)
      AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
    return GWEN_ERROR_GENERIC;
  }
  AH_Job_AddSigner(job, AB_User_GetUserId(u));

  /* enqueue job */
  ob=AH_Outbox_new(h);
  AH_Outbox_AddJob(ob, job);

  /* execute queue */
  rv=AH_Outbox_Execute(ob, ctx, withProgress, 0, doLock);
  if (rv) {
    GWEN_Gui_ProgressLog(0,
			 GWEN_LoggerLevel_Error,
			 I18N("Could not execute outbox."));
    AH_Outbox_free(ob);
    if (!nounmount && mounted)
      AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
    return rv;
  }

  /* check result */
  if (AH_Job_HasErrors(job)) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Job has errors");
    GWEN_Gui_ProgressLog(0,
			 GWEN_LoggerLevel_Error,
			 I18N("Job contains errors."));
    AH_Outbox_free(ob);
    if (!nounmount && mounted)
      AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
    return GWEN_ERROR_GENERIC;
  }
  else {
    rv=AH_Job_Commit(job, doLock);
    if (rv) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Could not commit result.\n");
      GWEN_Gui_ProgressLog(0,
			   GWEN_LoggerLevel_Error,
			   I18N("Could not commit result"));
      AH_Outbox_free(ob);
      if (!nounmount && mounted)
	AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
      return rv;
    }
  }

  GWEN_Gui_ProgressLog(0,
		       GWEN_LoggerLevel_Notice,
		       I18N("Keys sent"));

  AH_Outbox_free(ob);
  if (!nounmount && mounted)
    AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));

  return 0;
}



int AH_Provider_SendUserKeys(AB_PROVIDER *pro, AB_USER *u,
			     AB_IMEXPORTER_CONTEXT *ctx,
			     int withProgress, int nounmount, int doLock) {
  return AH_Provider_SendUserKeys2(pro, u, ctx, 0, withProgress, nounmount, doLock);
}



int AH_Provider_GetCert(AB_PROVIDER *pro,
			AB_USER *u,
			int withProgress, int nounmount, int doLock) {
  AB_BANKING *ab;
  AH_HBCI *h;
  int rv;
  AH_PROVIDER *hp;
  AH_DIALOG *dialog;
  uint32_t pid;

  assert(pro);
  hp=GWEN_INHERIT_GETDATA(AB_PROVIDER, AH_PROVIDER, pro);
  assert(hp);

  assert(u);

  ab=AB_Provider_GetBanking(pro);
  assert(ab);

  h=AH_Provider_GetHbci(pro);
  assert(h);

  pid=GWEN_Gui_ProgressStart(GWEN_GUI_PROGRESS_ALLOW_EMBED |
			     GWEN_GUI_PROGRESS_SHOW_PROGRESS |
			     GWEN_GUI_PROGRESS_SHOW_ABORT,
			     I18N("Getting Certificate"),
			     I18N("We are now asking the server for its "
				  "SSL certificate"),
			     GWEN_GUI_PROGRESS_NONE,
			     0);
  /* first try */
  dialog=AH_Dialog_new(u);
  assert(dialog);
  rv=AH_Dialog_TestServer_Https(dialog);
  AH_Dialog_Disconnect(dialog);
  AH_Dialog_free(dialog);

  if (rv) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Could not connect to server (%d)", rv);
    GWEN_Gui_ProgressLog(pid,
			 GWEN_LoggerLevel_Error,
			 I18N("Could not connect to server"));
    GWEN_Gui_ProgressEnd(pid);
    return rv;
  }

  GWEN_Gui_ProgressLog(pid,
		       GWEN_LoggerLevel_Error,
		       I18N("Got certificate"));
  GWEN_Gui_ProgressEnd(pid);

  return 0;
}



int AH_Provider_GetItanModes(AB_PROVIDER *pro, AB_USER *u,
                             AB_IMEXPORTER_CONTEXT *ctx,
			     int withProgress, int nounmount, int doLock) {
  AB_BANKING *ab;
  AH_HBCI *h;
  AH_JOB *job;
  AH_OUTBOX *ob;
  int rv;
  AH_PROVIDER *hp;
  const int *tm;
  char tbuf[256];

  assert(pro);
  hp=GWEN_INHERIT_GETDATA(AB_PROVIDER, AH_PROVIDER, pro);
  assert(hp);

  assert(u);

  ab=AB_Provider_GetBanking(pro);
  assert(ab);

  h=AH_Provider_GetHbci(pro);
  assert(h);

  if (doLock) {
    rv=AB_Banking_BeginExclUseUser(ab, u);
    if (rv<0) {
      DBG_INFO(AQHBCI_LOGDOMAIN,
	       "Could not lock customer [%s] (%d)",
	       AB_User_GetCustomerId(u), rv);
      snprintf(tbuf, sizeof(tbuf)-1,
	       I18N("Could not lock user %s (%d)"),
	       AB_User_GetUserId(u), rv);
      tbuf[sizeof(tbuf)-1]=0;
      GWEN_Gui_ProgressLog(0,
			   GWEN_LoggerLevel_Error,
			   tbuf);
      return rv;
    }
  }

  job=AH_Job_GetItanModes_new(u);
  if (!job) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Job not supported, should not happen");
    AB_Banking_EndExclUseUser(ab, u, 1);
    return GWEN_ERROR_GENERIC;
  }
  AH_Job_AddSigner(job, AB_User_GetUserId(u));

  ob=AH_Outbox_new(h);
  AH_Outbox_AddJob(ob, job);

  rv=AH_Outbox_Execute(ob, ctx, withProgress, 1, 0);
  if (rv) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Could not execute outbox.");
    if (doLock)
      AB_Banking_EndExclUseUser(ab, u, 1);
    AH_Outbox_free(ob);
    if (!nounmount)
      AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
    return rv;
  }

  tm=AH_Job_GetItanModes_GetModes(job);
  if (tm[0]==-1) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "No iTAN modes reported");
    GWEN_Gui_ProgressLog(0,
			 GWEN_LoggerLevel_Error,
			 I18N("No iTAN modes reported."));
    if (doLock)
      AB_Banking_EndExclUseUser(ab, u, 1);
    AH_Outbox_free(ob);
    if (!nounmount)
      AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
    return GWEN_ERROR_NO_DATA;
  }

  /* we have received tan methods, so there was a 3920 response. In this
   * special case we need to apply the job data, because otherwise we couldn't
   * fully connect to the server next time.
   */
  rv=AH_Job_Commit(job, 0);
  if (rv) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Could not commit result.\n");
    GWEN_Gui_ProgressLog(0,
			 GWEN_LoggerLevel_Error,
			 I18N("Could not commit result to the system"));
    if (doLock)
      AB_Banking_EndExclUseUser(ab, u, 1);
    AH_Outbox_free(ob);
    if (!nounmount)
      AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
    return rv;
  }

  if (doLock) {
    rv=AB_Banking_EndExclUseUser(ab, u, 0);
    if (rv<0) {
      DBG_INFO(AQHBCI_LOGDOMAIN,
	       "Could not unlock customer [%s] (%d)",
	       AB_User_GetCustomerId(u), rv);
      snprintf(tbuf, sizeof(tbuf)-1,
	       I18N("Could not unlock user %s (%d)"),
	       AB_User_GetUserId(u), rv);
      tbuf[sizeof(tbuf)-1]=0;
      GWEN_Gui_ProgressLog(0,
			   GWEN_LoggerLevel_Error,
			   tbuf);
      AB_Banking_EndExclUseUser(ab, u, 1);
      AH_Outbox_free(ob);
      if (!nounmount)
	AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
      return rv;
    }
  }

  AH_Outbox_free(ob);

  if (!nounmount)
    AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));

  return 0;
}



int AH_Provider_ChangePin(AB_PROVIDER *pro, AB_USER *u,
                          AB_IMEXPORTER_CONTEXT *ctx,
			  int withProgress, int nounmount, int doLock) {
  AB_BANKING *ab;
  AH_HBCI *h;
  AH_JOB *job;
  AH_OUTBOX *ob;
  int rv;
  AH_PROVIDER *hp;
  char pwbuf[32];

  assert(pro);
  hp=GWEN_INHERIT_GETDATA(AB_PROVIDER, AH_PROVIDER, pro);
  assert(hp);

  assert(u);

  ab=AB_Provider_GetBanking(pro);
  assert(ab);

  h=AH_Provider_GetHbci(pro);
  assert(h);

  memset(pwbuf, 0, sizeof(pwbuf));
  rv=GWEN_Gui_InputBox(GWEN_GUI_INPUT_FLAGS_NUMERIC |
		       GWEN_GUI_INPUT_FLAGS_CONFIRM,
		       I18N("Enter New Banking PIN"),
		       I18N("Please enter a new banking PIN.\n"
			    "You must only enter numbers, not letters.\n"
			    "<html>"
			    "<p>"
			    "Please enter a new banking PIN."
			    "</p>"
			    "<p>"
			    "You must only enter numbers, not letters."
			    "</p>"
			    "</html>"),
		       pwbuf,
		       0, 8, 0);

  job=AH_Job_ChangePin_new(u, pwbuf);
  if (!job) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Job not supported, should not happen");
    return GWEN_ERROR_GENERIC;
  }
  AH_Job_AddSigner(job, AB_User_GetUserId(u));

  ob=AH_Outbox_new(h);
  AH_Outbox_AddJob(ob, job);

  rv=AH_Outbox_Execute(ob, ctx, withProgress, nounmount, doLock);
  if (rv) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Could not execute outbox.\n");
    if (!nounmount)
      AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
    return rv;
  }

  if (AH_Job_HasErrors(job)) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Job has errors");
    // TODO: show errors
    AH_Outbox_free(ob);
    if (!nounmount)
      AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
    return GWEN_ERROR_GENERIC;
  }
  else {
    rv=AH_Job_Commit(job, doLock);
    if (rv) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Could not commit result.\n");
      AH_Outbox_free(ob);
      if (!nounmount)
	AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
      return rv;
    }
  }

  AH_Outbox_free(ob);

  if (!nounmount)
    AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));

  return 0;
}



int AH_Provider__HashRmd160(const uint8_t *p, unsigned int l, uint8_t *buf) {
  GWEN_MDIGEST *md;
  int rv;

  md=GWEN_MDigest_Rmd160_new();
  assert(md);
  rv=GWEN_MDigest_Begin(md);
  if (rv<0) {
    GWEN_MDigest_free(md);
    return rv;
  }
  rv=GWEN_MDigest_Update(md, p, l);
  if (rv<0) {
    GWEN_MDigest_free(md);
    return rv;
  }
  rv=GWEN_MDigest_End(md);
  if (rv<0) {
    GWEN_MDigest_free(md);
    return rv;
  }

  memmove(buf, GWEN_MDigest_GetDigestPtr(md), GWEN_MDigest_GetDigestSize(md));
  GWEN_MDigest_free(md);
  return 0;
}



int AH_Provider__HashSha256(const uint8_t *p, unsigned int l, uint8_t *buf) {
  GWEN_MDIGEST *md;
  int rv;

  md=GWEN_MDigest_Sha256_new();
  assert(md);
  rv=GWEN_MDigest_Begin(md);
  if (rv<0) {
    GWEN_MDigest_free(md);
    return rv;
  }
  rv=GWEN_MDigest_Update(md, p, l);
  if (rv<0) {
    GWEN_MDigest_free(md);
    return rv;
  }
  rv=GWEN_MDigest_End(md);
  if (rv<0) {
    GWEN_MDigest_free(md);
    return rv;
  }

  memmove(buf, GWEN_MDigest_GetDigestPtr(md), GWEN_MDigest_GetDigestSize(md));
  GWEN_MDigest_free(md);
  return 0;
}



int AH_Provider_GetIniLetterTxt(AB_PROVIDER *pro,
                                AB_USER *u,
                                int useBankKey,
                                int variant,
                                GWEN_BUFFER *lbuf,
				int nounmount) {
  if (variant==0) {
    switch(AH_User_GetRdhType(u)) {
    case 0:
    case 1: variant=1; break;
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 7:
    case 8:
    case 9:
    case 10: variant=2; break;
    default:
      DBG_ERROR(AQHBCI_LOGDOMAIN, "RDH mode %d not supported", AH_User_GetRdhType(u));
      return GWEN_ERROR_INVALID;
    }
  }

  switch(variant) {
  case 1: return AH_Provider_GetIniLetterTxt1(pro, u, useBankKey, lbuf, nounmount);
  case 2: return AH_Provider_GetIniLetterTxt2(pro, u, useBankKey, lbuf, nounmount);
  default:
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Variant %d not supported", variant);
    return GWEN_ERROR_INVALID;
  }
}


int AH_Provider_GetIniLetterHtml(AB_PROVIDER *pro,
				 AB_USER *u,
				 int useBankKey,
				 int variant,
				 GWEN_BUFFER *lbuf,
				 int nounmount) {
  if (variant==0) {
    switch(AH_User_GetRdhType(u)) {
    case 0:
    case 1: variant=1; break;
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 7:
    case 8:
    case 9:
    case 10: variant=2; break;
    default:
      DBG_ERROR(AQHBCI_LOGDOMAIN, "RDH mode %d not supported", AH_User_GetRdhType(u));
      return GWEN_ERROR_INVALID;
    }
  }

  switch(variant) {
  case 1: return AH_Provider_GetIniLetterHtml1(pro, u, useBankKey, lbuf, nounmount);
  case 2: return AH_Provider_GetIniLetterHtml2(pro, u, useBankKey, lbuf, nounmount);
  default:
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Variant %d not supported", variant);
    return GWEN_ERROR_INVALID;
  }
}



int AH_Provider_GetIniLetterTxt1(AB_PROVIDER *pro,
				 AB_USER *u,
				 int useBankKey,
				 GWEN_BUFFER *lbuf,
				 int nounmount) {
  AB_BANKING *ab;
  AH_HBCI *h;
  const void *p;
  unsigned int l;
  GWEN_BUFFER *bbuf;
  GWEN_BUFFER *keybuf;
  int i;
  GWEN_TIME *ti;
  char numbuf[32];
  char hashbuffer[21];
  AH_PROVIDER *hp;
  GWEN_CRYPT_TOKEN *ct;
  const GWEN_CRYPT_TOKEN_CONTEXT *cctx;
  const GWEN_CRYPT_TOKEN_KEYINFO *ki=NULL;
  uint32_t kid;
  int rv;

  assert(pro);
  hp=GWEN_INHERIT_GETDATA(AB_PROVIDER, AH_PROVIDER, pro);
  assert(hp);

  assert(u);

  ab=AB_Provider_GetBanking(pro);
  assert(ab);

  h=AH_Provider_GetHbci(pro);
  assert(h);

  rv=AB_Banking_GetCryptToken(AH_HBCI_GetBankingApi(h),
			      AH_User_GetTokenType(u),
			      AH_User_GetTokenName(u),
			      &ct);
  if (rv) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Could not get crypt token (%d)", rv);
    GWEN_Gui_ProgressLog(0,
			 GWEN_LoggerLevel_Error,
			 I18N("Error getting crypt token"));
    if (!nounmount)
      AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
    return rv;
  }

  /* open crypt token */
  rv=GWEN_Crypt_Token_Open(ct, 1, 0);
  if (rv) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Could not open crypt token (%d)", rv);
    GWEN_Gui_ProgressLog(0,
			 GWEN_LoggerLevel_Error,
			 I18N("Error opening crypt token"));
    if (!nounmount)
      AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
    return rv;
  }

  /* get context */
  cctx=GWEN_Crypt_Token_GetContext(ct, AH_User_GetTokenContextId(u), 0);
  if (!cctx) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "User context not found on crypt token");
    GWEN_Gui_ProgressLog(0,
			 GWEN_LoggerLevel_Error,
			 I18N("User context not found on crypt token"));
    if (!nounmount)
      AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
    return GWEN_ERROR_NOT_FOUND;
  }

  if (useBankKey) {
    /* get sign key info */
    kid=GWEN_Crypt_Token_Context_GetVerifyKeyId(cctx);
    if (kid) {
      ki=GWEN_Crypt_Token_GetKeyInfo(ct, kid,
				     GWEN_CRYPT_TOKEN_KEYFLAGS_HASMODULUS |
				     GWEN_CRYPT_TOKEN_KEYFLAGS_HASEXPONENT |
				     GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYVERSION |
				     GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYNUMBER,
				     0);
    }
    if (!ki ||
	!(GWEN_Crypt_Token_KeyInfo_GetFlags(ki) & GWEN_CRYPT_TOKEN_KEYFLAGS_HASMODULUS) |
	!(GWEN_Crypt_Token_KeyInfo_GetFlags(ki) & GWEN_CRYPT_TOKEN_KEYFLAGS_HASEXPONENT)) {
      kid=GWEN_Crypt_Token_Context_GetEncipherKeyId(cctx);
      if (kid) {
	ki=GWEN_Crypt_Token_GetKeyInfo(ct, kid,
				       GWEN_CRYPT_TOKEN_KEYFLAGS_HASMODULUS |
				       GWEN_CRYPT_TOKEN_KEYFLAGS_HASEXPONENT |
				       GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYVERSION |
				       GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYNUMBER,
				       0);
      }
    }
    if (!ki ||
	!(GWEN_Crypt_Token_KeyInfo_GetFlags(ki) & GWEN_CRYPT_TOKEN_KEYFLAGS_HASMODULUS) |
	!(GWEN_Crypt_Token_KeyInfo_GetFlags(ki) & GWEN_CRYPT_TOKEN_KEYFLAGS_HASEXPONENT)) {
      if (!nounmount)
	AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Server keys missing, please get them first");
      GWEN_Gui_ProgressLog(0,
			   GWEN_LoggerLevel_Error,
			   I18N("Server keys missing, "
				"please get them first"));
      return GWEN_ERROR_NOT_FOUND;
    }
  }
  else {
    /* get sign key info */
    kid=GWEN_Crypt_Token_Context_GetSignKeyId(cctx);
    if (kid) {
      ki=GWEN_Crypt_Token_GetKeyInfo(ct, kid,
				     GWEN_CRYPT_TOKEN_KEYFLAGS_HASMODULUS |
				     GWEN_CRYPT_TOKEN_KEYFLAGS_HASEXPONENT |
				     GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYVERSION |
				     GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYNUMBER,
				     0);
    }
    if (!ki ||
	!(GWEN_Crypt_Token_KeyInfo_GetFlags(ki) & GWEN_CRYPT_TOKEN_KEYFLAGS_HASMODULUS) |
	!(GWEN_Crypt_Token_KeyInfo_GetFlags(ki) & GWEN_CRYPT_TOKEN_KEYFLAGS_HASEXPONENT)) {
      if (!nounmount)
	AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
      DBG_ERROR(AQHBCI_LOGDOMAIN, "User keys missing, please generate them first");
      GWEN_Gui_ProgressLog(0,
			   GWEN_LoggerLevel_Error,
			   I18N("User keys missing, "
				"please generate them first"));
      return GWEN_ERROR_NOT_FOUND;
    }
  }


  keybuf=GWEN_Buffer_new(0, 257, 0, 1);

  /* prelude */
  GWEN_Buffer_AppendString(lbuf,
                           I18N("\n\n\nINI-Letter\n\n"));
  GWEN_Buffer_AppendString(lbuf,
                           I18N("Date           : "));
  ti=GWEN_CurrentTime();
  assert(ti);
  GWEN_Time_toString(ti, I18N("YYYY/MM/DD"), lbuf);
  GWEN_Buffer_AppendString(lbuf, "\n");
  GWEN_Buffer_AppendString(lbuf,
                           I18N("Time           : "));
  GWEN_Time_toString(ti, I18N("hh:mm:ss"), lbuf);
  GWEN_Buffer_AppendString(lbuf, "\n");

  if (useBankKey) {
    GWEN_Buffer_AppendString(lbuf,
                             I18N("Bank Code      : "));
    GWEN_Buffer_AppendString(lbuf, AB_User_GetBankCode(u));
  }
  else {
    GWEN_Buffer_AppendString(lbuf,
                             I18N("User           : "));
    GWEN_Buffer_AppendString(lbuf, AB_User_GetUserId(u));
  }

  GWEN_Buffer_AppendString(lbuf, "\n");
  GWEN_Buffer_AppendString(lbuf,
                           I18N("Key number     : "));
  snprintf(numbuf, sizeof(numbuf), "%d",
           GWEN_Crypt_Token_KeyInfo_GetKeyNumber(ki));
  GWEN_Buffer_AppendString(lbuf, numbuf);
  GWEN_Buffer_AppendString(lbuf, "\n");

  GWEN_Buffer_AppendString(lbuf,
                           I18N("Key version    : "));
  snprintf(numbuf, sizeof(numbuf), "%d",
	   GWEN_Crypt_Token_KeyInfo_GetKeyVersion(ki));
  GWEN_Buffer_AppendString(lbuf, numbuf);
  GWEN_Buffer_AppendString(lbuf, "\n");

  if (!useBankKey) {
    GWEN_Buffer_AppendString(lbuf,
                             I18N("Customer system: "));
    GWEN_Buffer_AppendString(lbuf, AH_HBCI_GetProductName(h));
    GWEN_Buffer_AppendString(lbuf, "\n");
  }

  GWEN_Buffer_AppendString(lbuf, "\n");
  GWEN_Buffer_AppendString(lbuf,
                           I18N("Public key for electronic signature"));
  GWEN_Buffer_AppendString(lbuf, "\n\n");

  GWEN_Buffer_AppendString(lbuf, "  ");
  GWEN_Buffer_AppendString(lbuf,
                           I18N("Exponent"));
  GWEN_Buffer_AppendString(lbuf, "\n\n");

  /* exponent */
  p=GWEN_Crypt_Token_KeyInfo_GetExponentData(ki);
  l=GWEN_Crypt_Token_KeyInfo_GetExponentLen(ki);
  if (!p || !l) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Bad key.");
    GWEN_Gui_ProgressLog(0, GWEN_LoggerLevel_Error,
                           I18N("Bad key"));
    return GWEN_ERROR_BAD_DATA;
  }

  bbuf=GWEN_Buffer_new(0, 97, 0, 1);
  GWEN_Buffer_AppendBytes(bbuf, p, l);
  GWEN_Buffer_Rewind(bbuf);
  if (l<96)
    GWEN_Buffer_FillLeftWithBytes(bbuf, 0, 96-l);
  p=GWEN_Buffer_GetStart(bbuf);
  l=GWEN_Buffer_GetUsedBytes(bbuf);
  for (i=0; i<6; i++) {
    GWEN_Buffer_AppendString(lbuf, "  ");
    if (GWEN_Text_ToHexBuffer(p, 16, lbuf, 2, ' ', 0)) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Error converting to hex??");
      abort();
    }
    p+=16;
    GWEN_Buffer_AppendString(lbuf, "\n");
  }

  GWEN_Buffer_FillWithBytes(keybuf, 0, 128-l);
  GWEN_Buffer_AppendBuffer(keybuf, bbuf);
  GWEN_Buffer_free(bbuf);

  /* modulus */
  GWEN_Buffer_AppendString(lbuf, "\n");
  GWEN_Buffer_AppendString(lbuf, "  ");
  GWEN_Buffer_AppendString(lbuf,
                           I18N("Modulus"));
  GWEN_Buffer_AppendString(lbuf, "\n\n");
  p=GWEN_Crypt_Token_KeyInfo_GetModulusData(ki);
  l=GWEN_Crypt_Token_KeyInfo_GetModulusLen(ki);
  if (!p || !l) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Bad key.");
    GWEN_Gui_ProgressLog(0, GWEN_LoggerLevel_Error,
                           I18N("Bad key"));
    return GWEN_ERROR_BAD_DATA;
  }

  bbuf=GWEN_Buffer_new(0, 97, 0, 1);
  GWEN_Buffer_AppendBytes(bbuf, p, l);
  GWEN_Buffer_Rewind(bbuf);
  if (l<96)
    GWEN_Buffer_FillLeftWithBytes(bbuf, 0, 96-l);
  p=GWEN_Buffer_GetStart(bbuf);
  l=GWEN_Buffer_GetUsedBytes(bbuf);
  for (i=0; i<6; i++) {
    GWEN_Buffer_AppendString(lbuf, "  ");
    if (GWEN_Text_ToHexBuffer(p, 16, lbuf, 2, ' ', 0)) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Error converting to hex??");
      abort();
    }
    p+=16;
    GWEN_Buffer_AppendString(lbuf, "\n");
  }

  GWEN_Buffer_FillWithBytes(keybuf, 0, 128-l);
  GWEN_Buffer_AppendBuffer(keybuf, bbuf);
  GWEN_Buffer_free(bbuf);

  GWEN_Buffer_AppendString(lbuf, "\n");
  GWEN_Buffer_AppendString(lbuf, "  ");
  GWEN_Buffer_AppendString(lbuf,
                           I18N("Hash"));
  GWEN_Buffer_AppendString(lbuf, "\n\n");
  rv=AH_Provider__HashRmd160((const uint8_t*)GWEN_Buffer_GetStart(keybuf),
			     GWEN_Buffer_GetUsedBytes(keybuf),
			     (uint8_t*)hashbuffer);
  if (rv) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Error hashing (%d)", rv);
    abort();
  }
  GWEN_Buffer_free(keybuf);

  GWEN_Buffer_AppendString(lbuf, "  ");
  if (GWEN_Text_ToHexBuffer(hashbuffer, 20, lbuf, 2, ' ', 0)) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Error converting to hex??");
    abort();
  }
  GWEN_Buffer_AppendString(lbuf, "\n");

  if (!useBankKey) {
    GWEN_Buffer_AppendString(lbuf, "\n\n");
    GWEN_Buffer_AppendString(lbuf,
                             I18N("I confirm that I created the above key "
                                  "for my electronic signature.\n"));
    GWEN_Buffer_AppendString(lbuf, "\n\n");
    GWEN_Buffer_AppendString(lbuf,
                             I18N("____________________________  "
                                  "____________________________\n"
                                  "Place, date                   "
                                  "Signature\n"));
  }

  return 0;
}



int AH_Provider_GetIniLetterHtml1(AB_PROVIDER *pro,
				  AB_USER *u,
				  int useBankKey,
				  GWEN_BUFFER *lbuf,
				  int nounmount) {
  AB_BANKING *ab;
  AH_HBCI *h;
  const void *p;
  unsigned int l;
  GWEN_BUFFER *bbuf;
  GWEN_BUFFER *keybuf;
  int i;
  GWEN_TIME *ti;
  char numbuf[32];
  char hashbuffer[21];
  AH_PROVIDER *hp;
  GWEN_CRYPT_TOKEN *ct;
  const GWEN_CRYPT_TOKEN_CONTEXT *cctx;
  const GWEN_CRYPT_TOKEN_KEYINFO *ki=NULL;
  uint32_t kid;
  int rv;

  assert(pro);
  hp=GWEN_INHERIT_GETDATA(AB_PROVIDER, AH_PROVIDER, pro);
  assert(hp);

  assert(u);

  ab=AB_Provider_GetBanking(pro);
  assert(ab);

  h=AH_Provider_GetHbci(pro);
  assert(h);

  rv=AB_Banking_GetCryptToken(AH_HBCI_GetBankingApi(h),
			      AH_User_GetTokenType(u),
			      AH_User_GetTokenName(u),
			      &ct);
  if (rv) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Could not get crypt token (%d)", rv);
    GWEN_Gui_ProgressLog(0,
			 GWEN_LoggerLevel_Error,
			 I18N("Error getting crypt token"));
    if (!nounmount)
      AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
    return rv;
  }

  /* open crypt token */
  rv=GWEN_Crypt_Token_Open(ct, 1, 0);
  if (rv) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Could not open crypt token (%d)", rv);
    GWEN_Gui_ProgressLog(0,
			 GWEN_LoggerLevel_Error,
			 I18N("Error opening crypt token"));
    if (!nounmount)
      AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
    return rv;
  }

  /* get context */
  cctx=GWEN_Crypt_Token_GetContext(ct, AH_User_GetTokenContextId(u), 0);
  if (!cctx) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "User context not found on crypt token");
    GWEN_Gui_ProgressLog(0,
			 GWEN_LoggerLevel_Error,
			 I18N("User context not found on crypt token"));
    if (!nounmount)
      AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
    return GWEN_ERROR_NOT_FOUND;
  }

  if (useBankKey) {
    /* get sign key info */
    kid=GWEN_Crypt_Token_Context_GetVerifyKeyId(cctx);
    if (kid) {
      ki=GWEN_Crypt_Token_GetKeyInfo(ct, kid,
				     GWEN_CRYPT_TOKEN_KEYFLAGS_HASMODULUS |
				     GWEN_CRYPT_TOKEN_KEYFLAGS_HASEXPONENT |
				     GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYVERSION |
				     GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYNUMBER,
				     0);
    }
    if (!ki ||
	!(GWEN_Crypt_Token_KeyInfo_GetFlags(ki) & GWEN_CRYPT_TOKEN_KEYFLAGS_HASMODULUS) |
	!(GWEN_Crypt_Token_KeyInfo_GetFlags(ki) & GWEN_CRYPT_TOKEN_KEYFLAGS_HASEXPONENT)) {
      kid=GWEN_Crypt_Token_Context_GetEncipherKeyId(cctx);
      if (kid) {
	ki=GWEN_Crypt_Token_GetKeyInfo(ct, kid,
				       GWEN_CRYPT_TOKEN_KEYFLAGS_HASMODULUS |
				       GWEN_CRYPT_TOKEN_KEYFLAGS_HASEXPONENT |
				       GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYVERSION |
				       GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYNUMBER,
				       0);
      }
    }
    if (!ki ||
	!(GWEN_Crypt_Token_KeyInfo_GetFlags(ki) & GWEN_CRYPT_TOKEN_KEYFLAGS_HASMODULUS) |
	!(GWEN_Crypt_Token_KeyInfo_GetFlags(ki) & GWEN_CRYPT_TOKEN_KEYFLAGS_HASEXPONENT)) {
      if (!nounmount)
	AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Server keys missing, please get them first");
      GWEN_Gui_ProgressLog(0,
			   GWEN_LoggerLevel_Error,
			   I18N("Server keys missing, "
				"please get them first"));
      return GWEN_ERROR_NOT_FOUND;
    }
  }
  else {
    /* get sign key info */
    kid=GWEN_Crypt_Token_Context_GetSignKeyId(cctx);
    if (kid) {
      ki=GWEN_Crypt_Token_GetKeyInfo(ct, kid,
				     GWEN_CRYPT_TOKEN_KEYFLAGS_HASMODULUS |
				     GWEN_CRYPT_TOKEN_KEYFLAGS_HASEXPONENT |
				     GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYVERSION |
				     GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYNUMBER,
				     0);
    }
    if (!ki ||
	!(GWEN_Crypt_Token_KeyInfo_GetFlags(ki) & GWEN_CRYPT_TOKEN_KEYFLAGS_HASMODULUS) |
	!(GWEN_Crypt_Token_KeyInfo_GetFlags(ki) & GWEN_CRYPT_TOKEN_KEYFLAGS_HASEXPONENT)) {
      if (!nounmount)
	AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
      DBG_ERROR(AQHBCI_LOGDOMAIN, "User keys missing, please generate them first");
      GWEN_Gui_ProgressLog(0,
			   GWEN_LoggerLevel_Error,
			   I18N("User keys missing, "
				"please generate them first"));
      return GWEN_ERROR_NOT_FOUND;
    }
  }

  keybuf=GWEN_Buffer_new(0, 257, 0, 1);

  /* prelude */
  GWEN_Buffer_AppendString(lbuf, "<h3>");
  GWEN_Buffer_AppendString(lbuf, I18N("INI-Letter"));
  GWEN_Buffer_AppendString(lbuf, "</h3>\n");
  GWEN_Buffer_AppendString(lbuf, "<table>\n");
  GWEN_Buffer_AppendString(lbuf, "<tr><td>\n");
  GWEN_Buffer_AppendString(lbuf, I18N("Date"));
  GWEN_Buffer_AppendString(lbuf, "</td><td>\n");
  ti=GWEN_CurrentTime();
  assert(ti);
  GWEN_Time_toString(ti, I18N("YYYY/MM/DD"), lbuf);
  GWEN_Buffer_AppendString(lbuf, I18N("Time"));
  GWEN_Time_toString(ti, I18N("hh:mm:ss"), lbuf);
  GWEN_Buffer_AppendString(lbuf, "</td></tr>\n");

  if (useBankKey) {
    GWEN_Buffer_AppendString(lbuf, "<tr><td>\n");
    GWEN_Buffer_AppendString(lbuf, I18N("Bank Code"));
    GWEN_Buffer_AppendString(lbuf, "</td><td>\n");
    GWEN_Buffer_AppendString(lbuf, AB_User_GetBankCode(u));
    GWEN_Buffer_AppendString(lbuf, "</td></tr>\n");
  }
  else {
    GWEN_Buffer_AppendString(lbuf, "<tr><td>\n");
    GWEN_Buffer_AppendString(lbuf, I18N("User"));
    GWEN_Buffer_AppendString(lbuf, "</td><td>\n");
    GWEN_Buffer_AppendString(lbuf, AB_User_GetUserId(u));
    GWEN_Buffer_AppendString(lbuf, "</td></tr>\n");
  }

  GWEN_Buffer_AppendString(lbuf, "<tr><td>\n");
  GWEN_Buffer_AppendString(lbuf, I18N("Key number"));
  GWEN_Buffer_AppendString(lbuf, "</td><td>\n");
  snprintf(numbuf, sizeof(numbuf), "%d",
	   GWEN_Crypt_Token_KeyInfo_GetKeyNumber(ki));
  GWEN_Buffer_AppendString(lbuf, numbuf);
  GWEN_Buffer_AppendString(lbuf, "</td></tr>\n");

  GWEN_Buffer_AppendString(lbuf, "<tr><td>\n");
  GWEN_Buffer_AppendString(lbuf, I18N("Key version"));
  GWEN_Buffer_AppendString(lbuf, "</td><td>\n");
  snprintf(numbuf, sizeof(numbuf), "%d",
	   GWEN_Crypt_Token_KeyInfo_GetKeyVersion(ki));
  GWEN_Buffer_AppendString(lbuf, numbuf);
  GWEN_Buffer_AppendString(lbuf, "</td></tr>\n");

  if (!useBankKey) {
    GWEN_Buffer_AppendString(lbuf, "<tr><td>\n");
    GWEN_Buffer_AppendString(lbuf, I18N("Customer system"));
    GWEN_Buffer_AppendString(lbuf, "</td><td>\n");
    GWEN_Buffer_AppendString(lbuf, AH_HBCI_GetProductName(h));
    GWEN_Buffer_AppendString(lbuf, "</td></tr>\n");
  }
  GWEN_Buffer_AppendString(lbuf, "</table>\n");

  GWEN_Buffer_AppendString(lbuf, "<h3>");
  GWEN_Buffer_AppendString(lbuf, I18N("Public key for electronic signature"));
  GWEN_Buffer_AppendString(lbuf, "</h3>\n");

  GWEN_Buffer_AppendString(lbuf, "<h4>");
  GWEN_Buffer_AppendString(lbuf, I18N("Exponent"));
  GWEN_Buffer_AppendString(lbuf, "</h4>\n");

  /* exponent */
  p=GWEN_Crypt_Token_KeyInfo_GetExponentData(ki);
  l=GWEN_Crypt_Token_KeyInfo_GetExponentLen(ki);
  if (!p || !l) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Bad key.");
    GWEN_Gui_ProgressLog(0, GWEN_LoggerLevel_Error,
                           I18N("Bad key"));
    return GWEN_ERROR_BAD_DATA;
  }

  GWEN_Buffer_AppendString(lbuf, "<font face=fixed>\n");
  bbuf=GWEN_Buffer_new(0, 97, 0, 1);
  GWEN_Buffer_AppendBytes(bbuf, p, l);
  GWEN_Buffer_Rewind(bbuf);
  if (l<96)
    GWEN_Buffer_FillLeftWithBytes(bbuf, 0, 96-l);
  p=GWEN_Buffer_GetStart(bbuf);
  l=GWEN_Buffer_GetUsedBytes(bbuf);
  for (i=0; i<6; i++) {
    GWEN_Buffer_AppendString(lbuf, "  ");
    if (GWEN_Text_ToHexBuffer(p, 16, lbuf, 2, ' ', 0)) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Error converting to hex??");
      abort();
    }
    p+=16;
    GWEN_Buffer_AppendString(lbuf, "<br>\n");
  }

  GWEN_Buffer_FillWithBytes(keybuf, 0, 128-l);
  GWEN_Buffer_AppendBuffer(keybuf, bbuf);
  GWEN_Buffer_free(bbuf);
  GWEN_Buffer_AppendString(lbuf, "</font>\n");
  GWEN_Buffer_AppendString(lbuf, "<br>\n");

  /* modulus */
  GWEN_Buffer_AppendString(lbuf, "\n");
  GWEN_Buffer_AppendString(lbuf, "<h4>");
  GWEN_Buffer_AppendString(lbuf, I18N("Modulus"));
  GWEN_Buffer_AppendString(lbuf, "</h4>\n");
  p=GWEN_Crypt_Token_KeyInfo_GetModulusData(ki);
  l=GWEN_Crypt_Token_KeyInfo_GetModulusLen(ki);
  if (!p || !l) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Bad key.");
    GWEN_Gui_ProgressLog(0, GWEN_LoggerLevel_Error,
                           I18N("Bad key"));
    return GWEN_ERROR_BAD_DATA;
  }

  GWEN_Buffer_AppendString(lbuf, "<font face=fixed>\n");
  bbuf=GWEN_Buffer_new(0, 97, 0, 1);
  GWEN_Buffer_AppendBytes(bbuf, p, l);
  GWEN_Buffer_Rewind(bbuf);
  if (l<96)
    GWEN_Buffer_FillLeftWithBytes(bbuf, 0, 96-l);
  p=GWEN_Buffer_GetStart(bbuf);
  l=GWEN_Buffer_GetUsedBytes(bbuf);
  for (i=0; i<6; i++) {
    GWEN_Buffer_AppendString(lbuf, "  ");
    if (GWEN_Text_ToHexBuffer(p, 16, lbuf, 2, ' ', 0)) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Error converting to hex??");
      abort();
    }
    p+=16;
    GWEN_Buffer_AppendString(lbuf, "<br>\n");
  }

  GWEN_Buffer_FillWithBytes(keybuf, 0, 128-l);
  GWEN_Buffer_AppendBuffer(keybuf, bbuf);
  GWEN_Buffer_free(bbuf);
  GWEN_Buffer_AppendString(lbuf, "</font>\n");
  GWEN_Buffer_AppendString(lbuf, "<br>\n");

  GWEN_Buffer_AppendString(lbuf, "<h4>");
  GWEN_Buffer_AppendString(lbuf, I18N("Hash"));
  GWEN_Buffer_AppendString(lbuf, "</h4>\n");
  GWEN_Buffer_AppendString(lbuf, "<font face=fixed>\n");
  rv=AH_Provider__HashRmd160((const uint8_t*)GWEN_Buffer_GetStart(keybuf),
			     GWEN_Buffer_GetUsedBytes(keybuf),
			     (uint8_t*)hashbuffer);
  if (rv) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Error hashing (%d)", rv);
    abort();
  }
  GWEN_Buffer_free(keybuf);

  GWEN_Buffer_AppendString(lbuf, "  ");
  if (GWEN_Text_ToHexBuffer(hashbuffer, 20, lbuf, 2, ' ', 0)) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Error converting to hex??");
    abort();
  }
  GWEN_Buffer_AppendString(lbuf, "</font>\n");
  GWEN_Buffer_AppendString(lbuf, "<br>\n");

  if (!useBankKey) {
    GWEN_Buffer_AppendString(lbuf, "<br><br>\n");
    GWEN_Buffer_AppendString(lbuf,
                             I18N("I confirm that I created the above key "
                                  "for my electronic signature.\n"));
    GWEN_Buffer_AppendString(lbuf, "<br><br>\n");
    GWEN_Buffer_AppendString(lbuf, "<table>\n");
    GWEN_Buffer_AppendString(lbuf, "<tr><td>\n");
    GWEN_Buffer_AppendString(lbuf, "____________________________  ");
    GWEN_Buffer_AppendString(lbuf, "</td><td>\n");
    GWEN_Buffer_AppendString(lbuf, "____________________________  ");
    GWEN_Buffer_AppendString(lbuf, "</td></tr><tr><td>\n");
    GWEN_Buffer_AppendString(lbuf, I18N("Place, date"));
    GWEN_Buffer_AppendString(lbuf, "</td><td>\n");
    GWEN_Buffer_AppendString(lbuf, I18N("Signature"));
    GWEN_Buffer_AppendString(lbuf, "</td></tr></table>\n");
    GWEN_Buffer_AppendString(lbuf, "<br>\n");
  }

  return 0;
}



int AH_Provider_GetIniLetterTxt2(AB_PROVIDER *pro,
				 AB_USER *u,
				 int useBankKey,
				 GWEN_BUFFER *lbuf,
				 int nounmount) {
  AB_BANKING *ab;
  AH_HBCI *h;
  const void *p;
  unsigned int l;
  unsigned int modLen;
  GWEN_BUFFER *bbuf;
  GWEN_BUFFER *keybuf;
  int i;
  GWEN_TIME *ti;
  char numbuf[32];
  char hashbuffer[33];
  AH_PROVIDER *hp;
  GWEN_CRYPT_TOKEN *ct;
  const GWEN_CRYPT_TOKEN_CONTEXT *cctx;
  const GWEN_CRYPT_TOKEN_KEYINFO *ki=NULL;
  uint32_t kid;
  int rv;

  assert(pro);
  hp=GWEN_INHERIT_GETDATA(AB_PROVIDER, AH_PROVIDER, pro);
  assert(hp);

  assert(u);

  ab=AB_Provider_GetBanking(pro);
  assert(ab);

  h=AH_Provider_GetHbci(pro);
  assert(h);

  rv=AB_Banking_GetCryptToken(AH_HBCI_GetBankingApi(h),
			      AH_User_GetTokenType(u),
			      AH_User_GetTokenName(u),
			      &ct);
  if (rv) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Could not get crypt token (%d)", rv);
    GWEN_Gui_ProgressLog(0,
			 GWEN_LoggerLevel_Error,
			 I18N("Error getting crypt token"));
    if (!nounmount)
      AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
    return rv;
  }

  /* open crypt token */
  rv=GWEN_Crypt_Token_Open(ct, 1, 0);
  if (rv) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Could not open crypt token (%d)", rv);
    GWEN_Gui_ProgressLog(0,
			 GWEN_LoggerLevel_Error,
			 I18N("Error opening crypt token"));
    if (!nounmount)
      AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
    return rv;
  }

  /* get context */
  cctx=GWEN_Crypt_Token_GetContext(ct, AH_User_GetTokenContextId(u), 0);
  if (!cctx) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "User context not found on crypt token");
    GWEN_Gui_ProgressLog(0,
			 GWEN_LoggerLevel_Error,
			 I18N("User context not found on crypt token"));
    if (!nounmount)
      AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
    return GWEN_ERROR_NOT_FOUND;
  }

  if (useBankKey) {
    /* get sign key info */
    kid=GWEN_Crypt_Token_Context_GetVerifyKeyId(cctx);
    if (kid) {
      ki=GWEN_Crypt_Token_GetKeyInfo(ct, kid,
				     GWEN_CRYPT_TOKEN_KEYFLAGS_HASMODULUS |
				     GWEN_CRYPT_TOKEN_KEYFLAGS_HASEXPONENT |
				     GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYVERSION |
				     GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYNUMBER,
				     0);
    }
    if (!ki ||
	!(GWEN_Crypt_Token_KeyInfo_GetFlags(ki) & GWEN_CRYPT_TOKEN_KEYFLAGS_HASMODULUS) |
	!(GWEN_Crypt_Token_KeyInfo_GetFlags(ki) & GWEN_CRYPT_TOKEN_KEYFLAGS_HASEXPONENT)) {
      kid=GWEN_Crypt_Token_Context_GetEncipherKeyId(cctx);
      if (kid) {
	ki=GWEN_Crypt_Token_GetKeyInfo(ct, kid,
				       GWEN_CRYPT_TOKEN_KEYFLAGS_HASMODULUS |
				       GWEN_CRYPT_TOKEN_KEYFLAGS_HASEXPONENT |
				       GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYVERSION |
				       GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYNUMBER,
				       0);
      }
    }
    if (!ki ||
	!(GWEN_Crypt_Token_KeyInfo_GetFlags(ki) & GWEN_CRYPT_TOKEN_KEYFLAGS_HASMODULUS) |
	!(GWEN_Crypt_Token_KeyInfo_GetFlags(ki) & GWEN_CRYPT_TOKEN_KEYFLAGS_HASEXPONENT)) {
      if (!nounmount)
	AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Server keys missing, please get them first");
      GWEN_Gui_ProgressLog(0,
			   GWEN_LoggerLevel_Error,
			   I18N("Server keys missing, "
				"please get them first"));
      return GWEN_ERROR_NOT_FOUND;
    }
  }
  else {
    /* get sign key info */
    kid=GWEN_Crypt_Token_Context_GetSignKeyId(cctx);
    if (kid) {
      ki=GWEN_Crypt_Token_GetKeyInfo(ct, kid,
				     GWEN_CRYPT_TOKEN_KEYFLAGS_HASMODULUS |
				     GWEN_CRYPT_TOKEN_KEYFLAGS_HASEXPONENT |
				     GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYVERSION |
				     GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYNUMBER,
				     0);
    }
    if (!ki ||
	!(GWEN_Crypt_Token_KeyInfo_GetFlags(ki) & GWEN_CRYPT_TOKEN_KEYFLAGS_HASMODULUS) |
	!(GWEN_Crypt_Token_KeyInfo_GetFlags(ki) & GWEN_CRYPT_TOKEN_KEYFLAGS_HASEXPONENT)) {
      if (!nounmount)
	AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
      DBG_ERROR(AQHBCI_LOGDOMAIN, "User keys missing, please generate them first");
      GWEN_Gui_ProgressLog(0,
			   GWEN_LoggerLevel_Error,
			   I18N("User keys missing, "
				"please generate them first"));
      return GWEN_ERROR_NOT_FOUND;
    }
  }

#if 0
  modLen=GWEN_Crypt_Token_KeyInfo_GetModulusLen(ki);
#else
  /* use the real modulus length */
  modLen=GWEN_Crypt_Token_KeyInfo_GetModulusLen(ki);
#endif

  keybuf=GWEN_Buffer_new(0, (modLen*2)+1, 0, 1);

  /* prelude */
  GWEN_Buffer_AppendString(lbuf,
                           I18N("\n\n\nINI-Letter\n\n"));
  GWEN_Buffer_AppendString(lbuf,
                           I18N("Date           : "));
  ti=GWEN_CurrentTime();
  assert(ti);
  GWEN_Time_toString(ti, I18N("YYYY/MM/DD"), lbuf);
  GWEN_Buffer_AppendString(lbuf, "\n");
  GWEN_Buffer_AppendString(lbuf,
                           I18N("Time           : "));
  GWEN_Time_toString(ti, I18N("hh:mm:ss"), lbuf);
  GWEN_Buffer_AppendString(lbuf, "\n");

  if (useBankKey) {
    GWEN_Buffer_AppendString(lbuf,
                             I18N("Bank Code      : "));
    GWEN_Buffer_AppendString(lbuf, AB_User_GetBankCode(u));
  }
  else {
    GWEN_Buffer_AppendString(lbuf,
                             I18N("User           : "));
    GWEN_Buffer_AppendString(lbuf, AB_User_GetUserId(u));
  }

  GWEN_Buffer_AppendString(lbuf, "\n");
  GWEN_Buffer_AppendString(lbuf,
                           I18N("Key number     : "));
  snprintf(numbuf, sizeof(numbuf), "%d",
           GWEN_Crypt_Token_KeyInfo_GetKeyNumber(ki));
  GWEN_Buffer_AppendString(lbuf, numbuf);
  GWEN_Buffer_AppendString(lbuf, "\n");

  GWEN_Buffer_AppendString(lbuf,
                           I18N("Key version    : "));
  snprintf(numbuf, sizeof(numbuf), "%d",
	   GWEN_Crypt_Token_KeyInfo_GetKeyVersion(ki));
  GWEN_Buffer_AppendString(lbuf, numbuf);
  GWEN_Buffer_AppendString(lbuf, "\n");

  if (!useBankKey) {
    GWEN_Buffer_AppendString(lbuf,
                             I18N("Customer system: "));
    GWEN_Buffer_AppendString(lbuf, AH_HBCI_GetProductName(h));
    GWEN_Buffer_AppendString(lbuf, "\n");
  }

  GWEN_Buffer_AppendString(lbuf, "\n");
  GWEN_Buffer_AppendString(lbuf,
                           I18N("Public key for electronic signature"));
  GWEN_Buffer_AppendString(lbuf, "\n\n");

  GWEN_Buffer_AppendString(lbuf, "  ");
  GWEN_Buffer_AppendString(lbuf,
                           I18N("Exponent"));
  GWEN_Buffer_AppendString(lbuf, "\n\n");

  /* exponent */
  p=GWEN_Crypt_Token_KeyInfo_GetExponentData(ki);
  l=GWEN_Crypt_Token_KeyInfo_GetExponentLen(ki);
  if (!p || !l) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Bad key.");
    GWEN_Gui_ProgressLog(0, GWEN_LoggerLevel_Error,
                           I18N("Bad key"));
    return GWEN_ERROR_BAD_DATA;
  }

  bbuf=GWEN_Buffer_new(0, modLen+1, 0, 1);
  GWEN_Buffer_AppendBytes(bbuf, p, l);
  GWEN_Buffer_Rewind(bbuf);
  if (l<modLen)
    GWEN_Buffer_FillLeftWithBytes(bbuf, 0, modLen-l);
  p=GWEN_Buffer_GetStart(bbuf);
  l=GWEN_Buffer_GetUsedBytes(bbuf);
  for (i=0; i<modLen; i+=16) {
    int rl=modLen-i;
    if (rl>16)
      rl=16;

    GWEN_Buffer_AppendString(lbuf, "  ");
    if (GWEN_Text_ToHexBuffer(p, rl, lbuf, 2, ' ', 0)) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Error converting to hex??");
      abort();
    }
    p+=rl;
    GWEN_Buffer_AppendString(lbuf, "\n");
  }

  GWEN_Buffer_AppendBuffer(keybuf, bbuf);
  GWEN_Buffer_free(bbuf);

  /* modulus */
  GWEN_Buffer_AppendString(lbuf, "\n");
  GWEN_Buffer_AppendString(lbuf, "  ");
  GWEN_Buffer_AppendString(lbuf,
                           I18N("Modulus"));
  GWEN_Buffer_AppendString(lbuf, "\n\n");
  p=GWEN_Crypt_Token_KeyInfo_GetModulusData(ki);
  l=GWEN_Crypt_Token_KeyInfo_GetModulusLen(ki);
  if (!p || !l) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Bad key.");
    GWEN_Gui_ProgressLog(0, GWEN_LoggerLevel_Error,
                           I18N("Bad key"));
    return GWEN_ERROR_BAD_DATA;
  }

  bbuf=GWEN_Buffer_new(0, modLen+1, 0, 1);
  GWEN_Buffer_AppendBytes(bbuf, p, l);
  GWEN_Buffer_Rewind(bbuf);
  p=GWEN_Buffer_GetStart(bbuf);
  l=GWEN_Buffer_GetUsedBytes(bbuf);
  for (i=0; i<modLen; i+=16) {
    int rl=modLen-i;
    if (rl>16)
      rl=16;

    GWEN_Buffer_AppendString(lbuf, "  ");
    if (GWEN_Text_ToHexBuffer(p, rl, lbuf, 2, ' ', 0)) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Error converting to hex??");
      abort();
    }
    p+=rl;
    GWEN_Buffer_AppendString(lbuf, "\n");
  }

  GWEN_Buffer_AppendBuffer(keybuf, bbuf);
  GWEN_Buffer_free(bbuf);

  GWEN_Buffer_AppendString(lbuf, "\n");
  GWEN_Buffer_AppendString(lbuf, "  ");
  GWEN_Buffer_AppendString(lbuf,
                           I18N("Hash (RMD-160)"));
  GWEN_Buffer_AppendString(lbuf, "\n\n");
  rv=AH_Provider__HashRmd160((const uint8_t*)GWEN_Buffer_GetStart(keybuf),
			     GWEN_Buffer_GetUsedBytes(keybuf),
			     (uint8_t*)hashbuffer);
  if (rv) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Error hashing (%d)", rv);
    abort();
  }

  GWEN_Buffer_AppendString(lbuf, "  ");
  if (GWEN_Text_ToHexBuffer(hashbuffer, 10, lbuf, 2, ' ', 0)) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Error converting to hex??");
    abort();
  }
  GWEN_Buffer_AppendString(lbuf, "\n  ");
  if (GWEN_Text_ToHexBuffer(hashbuffer+10, 10, lbuf, 2, ' ', 0)) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Error converting to hex??");
    abort();
  }
  GWEN_Buffer_AppendString(lbuf, "\n");

  GWEN_Buffer_AppendString(lbuf, "\n");
  GWEN_Buffer_AppendString(lbuf, "  ");
  GWEN_Buffer_AppendString(lbuf,
                           I18N("Hash (SHA-256)"));
  GWEN_Buffer_AppendString(lbuf, "\n\n");
  rv=AH_Provider__HashSha256((const uint8_t*)GWEN_Buffer_GetStart(keybuf),
			     GWEN_Buffer_GetUsedBytes(keybuf),
			     (uint8_t*)hashbuffer);
  if (rv) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Error hashing (%d)", rv);
    abort();
  }
  GWEN_Buffer_free(keybuf);

  GWEN_Buffer_AppendString(lbuf, "  ");
  if (GWEN_Text_ToHexBuffer(hashbuffer, 16, lbuf, 2, ' ', 0)) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Error converting to hex??");
    abort();
  }
  GWEN_Buffer_AppendString(lbuf, "\n  ");
  if (GWEN_Text_ToHexBuffer(hashbuffer+16, 16, lbuf, 2, ' ', 0)) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Error converting to hex??");
    abort();
  }
  GWEN_Buffer_AppendString(lbuf, "\n");

  if (!useBankKey) {
    GWEN_Buffer_AppendString(lbuf, "\n\n");
    GWEN_Buffer_AppendString(lbuf,
                             I18N("I confirm that I created the above key "
                                  "for my electronic signature.\n"));
    GWEN_Buffer_AppendString(lbuf, "\n\n");
    GWEN_Buffer_AppendString(lbuf,
                             I18N("____________________________  "
                                  "____________________________\n"
                                  "Place, date                   "
                                  "Signature\n"));
  }

  return 0;
}


int AH_Provider_GetIniLetterHtml2(AB_PROVIDER *pro,
				  AB_USER *u,
				  int useBankKey,
				  GWEN_BUFFER *lbuf,
				  int nounmount) {
  AB_BANKING *ab;
  AH_HBCI *h;
  const void *p;
  unsigned int l;
  GWEN_BUFFER *bbuf;
  GWEN_BUFFER *keybuf;
  int i;
  GWEN_TIME *ti;
  char numbuf[32];
  char hashbuffer[33];
  AH_PROVIDER *hp;
  GWEN_CRYPT_TOKEN *ct;
  const GWEN_CRYPT_TOKEN_CONTEXT *cctx;
  const GWEN_CRYPT_TOKEN_KEYINFO *ki=NULL;
  uint32_t kid;
  int rv;
  int modLen;

  assert(pro);
  hp=GWEN_INHERIT_GETDATA(AB_PROVIDER, AH_PROVIDER, pro);
  assert(hp);

  assert(u);

  ab=AB_Provider_GetBanking(pro);
  assert(ab);

  h=AH_Provider_GetHbci(pro);
  assert(h);

  rv=AB_Banking_GetCryptToken(AH_HBCI_GetBankingApi(h),
			      AH_User_GetTokenType(u),
			      AH_User_GetTokenName(u),
			      &ct);
  if (rv) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Could not get crypt token (%d)", rv);
    GWEN_Gui_ProgressLog(0,
			 GWEN_LoggerLevel_Error,
			 I18N("Error getting crypt token"));
    if (!nounmount)
      AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
    return rv;
  }

  /* open crypt token */
  rv=GWEN_Crypt_Token_Open(ct, 1, 0);
  if (rv) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Could not open crypt token (%d)", rv);
    GWEN_Gui_ProgressLog(0,
			 GWEN_LoggerLevel_Error,
			 I18N("Error opening crypt token"));
    if (!nounmount)
      AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
    return rv;
  }

  /* get context */
  cctx=GWEN_Crypt_Token_GetContext(ct, AH_User_GetTokenContextId(u), 0);
  if (!cctx) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "User context not found on crypt token");
    GWEN_Gui_ProgressLog(0,
			 GWEN_LoggerLevel_Error,
			 I18N("User context not found on crypt token"));
    if (!nounmount)
      AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
    return GWEN_ERROR_NOT_FOUND;
  }

  if (useBankKey) {
    /* get sign key info */
    kid=GWEN_Crypt_Token_Context_GetVerifyKeyId(cctx);
    if (kid) {
      ki=GWEN_Crypt_Token_GetKeyInfo(ct, kid,
				     GWEN_CRYPT_TOKEN_KEYFLAGS_HASMODULUS |
				     GWEN_CRYPT_TOKEN_KEYFLAGS_HASEXPONENT |
				     GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYVERSION |
				     GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYNUMBER,
				     0);
    }
    if (!ki ||
	!(GWEN_Crypt_Token_KeyInfo_GetFlags(ki) & GWEN_CRYPT_TOKEN_KEYFLAGS_HASMODULUS) |
	!(GWEN_Crypt_Token_KeyInfo_GetFlags(ki) & GWEN_CRYPT_TOKEN_KEYFLAGS_HASEXPONENT)) {
      kid=GWEN_Crypt_Token_Context_GetEncipherKeyId(cctx);
      if (kid) {
	ki=GWEN_Crypt_Token_GetKeyInfo(ct, kid,
				       GWEN_CRYPT_TOKEN_KEYFLAGS_HASMODULUS |
				       GWEN_CRYPT_TOKEN_KEYFLAGS_HASEXPONENT |
				       GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYVERSION |
				       GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYNUMBER,
				       0);
      }
    }
    if (!ki ||
	!(GWEN_Crypt_Token_KeyInfo_GetFlags(ki) & GWEN_CRYPT_TOKEN_KEYFLAGS_HASMODULUS) |
	!(GWEN_Crypt_Token_KeyInfo_GetFlags(ki) & GWEN_CRYPT_TOKEN_KEYFLAGS_HASEXPONENT)) {
      if (!nounmount)
	AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Server keys missing, please get them first");
      GWEN_Gui_ProgressLog(0,
			   GWEN_LoggerLevel_Error,
			   I18N("Server keys missing, "
				"please get them first"));
      return GWEN_ERROR_NOT_FOUND;
    }
  }
  else {
    /* get sign key info */
    kid=GWEN_Crypt_Token_Context_GetSignKeyId(cctx);
    if (kid) {
      ki=GWEN_Crypt_Token_GetKeyInfo(ct, kid,
				     GWEN_CRYPT_TOKEN_KEYFLAGS_HASMODULUS |
				     GWEN_CRYPT_TOKEN_KEYFLAGS_HASEXPONENT |
				     GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYVERSION |
				     GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYNUMBER,
				     0);
    }
    if (!ki ||
	!(GWEN_Crypt_Token_KeyInfo_GetFlags(ki) & GWEN_CRYPT_TOKEN_KEYFLAGS_HASMODULUS) |
	!(GWEN_Crypt_Token_KeyInfo_GetFlags(ki) & GWEN_CRYPT_TOKEN_KEYFLAGS_HASEXPONENT)) {
      if (!nounmount)
	AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
      DBG_ERROR(AQHBCI_LOGDOMAIN, "User keys missing, please generate them first");
      GWEN_Gui_ProgressLog(0,
			   GWEN_LoggerLevel_Error,
			   I18N("User keys missing, "
				"please generate them first"));
      return GWEN_ERROR_NOT_FOUND;
    }
  }

#if 0
  modLen=GWEN_Crypt_Token_KeyInfo_GetModulusLen(ki);
#else
  /* use the real modulus length */
  modLen=GWEN_Crypt_Token_KeyInfo_GetModulusLen(ki);
#endif

  keybuf=GWEN_Buffer_new(0, 257, 0, 1);

  /* prelude */
  GWEN_Buffer_AppendString(lbuf, "<h3>");
  GWEN_Buffer_AppendString(lbuf, I18N("INI-Letter"));
  GWEN_Buffer_AppendString(lbuf, "</h3>\n");
  GWEN_Buffer_AppendString(lbuf, "<table>\n");
  GWEN_Buffer_AppendString(lbuf, "<tr><td>\n");
  GWEN_Buffer_AppendString(lbuf, I18N("Date"));
  GWEN_Buffer_AppendString(lbuf, "</td><td>\n");
  ti=GWEN_CurrentTime();
  assert(ti);
  GWEN_Time_toString(ti, I18N("YYYY/MM/DD"), lbuf);
  GWEN_Buffer_AppendString(lbuf, I18N("Time"));
  GWEN_Time_toString(ti, I18N("hh:mm:ss"), lbuf);
  GWEN_Buffer_AppendString(lbuf, "</td></tr>\n");

  if (useBankKey) {
    GWEN_Buffer_AppendString(lbuf, "<tr><td>\n");
    GWEN_Buffer_AppendString(lbuf, I18N("Bank Code"));
    GWEN_Buffer_AppendString(lbuf, "</td><td>\n");
    GWEN_Buffer_AppendString(lbuf, AB_User_GetBankCode(u));
    GWEN_Buffer_AppendString(lbuf, "</td></tr>\n");
  }
  else {
    GWEN_Buffer_AppendString(lbuf, "<tr><td>\n");
    GWEN_Buffer_AppendString(lbuf, I18N("User"));
    GWEN_Buffer_AppendString(lbuf, "</td><td>\n");
    GWEN_Buffer_AppendString(lbuf, AB_User_GetUserId(u));
    GWEN_Buffer_AppendString(lbuf, "</td></tr>\n");
  }

  GWEN_Buffer_AppendString(lbuf, "<tr><td>\n");
  GWEN_Buffer_AppendString(lbuf, I18N("Key number"));
  GWEN_Buffer_AppendString(lbuf, "</td><td>\n");
  snprintf(numbuf, sizeof(numbuf), "%d",
	   GWEN_Crypt_Token_KeyInfo_GetKeyNumber(ki));
  GWEN_Buffer_AppendString(lbuf, numbuf);
  GWEN_Buffer_AppendString(lbuf, "</td></tr>\n");

  GWEN_Buffer_AppendString(lbuf, "<tr><td>\n");
  GWEN_Buffer_AppendString(lbuf, I18N("Key version"));
  GWEN_Buffer_AppendString(lbuf, "</td><td>\n");
  snprintf(numbuf, sizeof(numbuf), "%d",
	   GWEN_Crypt_Token_KeyInfo_GetKeyVersion(ki));
  GWEN_Buffer_AppendString(lbuf, numbuf);
  GWEN_Buffer_AppendString(lbuf, "</td></tr>\n");

  if (!useBankKey) {
    GWEN_Buffer_AppendString(lbuf, "<tr><td>\n");
    GWEN_Buffer_AppendString(lbuf, I18N("Customer system"));
    GWEN_Buffer_AppendString(lbuf, "</td><td>\n");
    GWEN_Buffer_AppendString(lbuf, AH_HBCI_GetProductName(h));
    GWEN_Buffer_AppendString(lbuf, "</td></tr>\n");
  }
  GWEN_Buffer_AppendString(lbuf, "</table>\n");

  GWEN_Buffer_AppendString(lbuf, "<h3>");
  GWEN_Buffer_AppendString(lbuf, I18N("Public key for electronic signature"));
  GWEN_Buffer_AppendString(lbuf, "</h3>\n");

  GWEN_Buffer_AppendString(lbuf, "<h4>");
  GWEN_Buffer_AppendString(lbuf, I18N("Exponent"));
  GWEN_Buffer_AppendString(lbuf, "</h4>\n");

  /* exponent */
  p=GWEN_Crypt_Token_KeyInfo_GetExponentData(ki);
  l=GWEN_Crypt_Token_KeyInfo_GetExponentLen(ki);
  if (!p || !l) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Bad key.");
    GWEN_Gui_ProgressLog(0, GWEN_LoggerLevel_Error,
                           I18N("Bad key"));
    return GWEN_ERROR_BAD_DATA;
  }

  GWEN_Buffer_AppendString(lbuf, "<font face=fixed>\n");
  bbuf=GWEN_Buffer_new(0, modLen+1, 0, 1);
  GWEN_Buffer_AppendBytes(bbuf, p, l);
  GWEN_Buffer_Rewind(bbuf);
  if (l<modLen)
    GWEN_Buffer_FillLeftWithBytes(bbuf, 0, modLen-l);
  p=GWEN_Buffer_GetStart(bbuf);
  l=GWEN_Buffer_GetUsedBytes(bbuf);
  for (i=0; i<modLen; i+=16) {
    int rl=modLen-i;
    if (rl>16)
      rl=16;

    GWEN_Buffer_AppendString(lbuf, "  ");
    if (GWEN_Text_ToHexBuffer(p, rl, lbuf, 2, ' ', 0)) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Error converting to hex??");
      abort();
    }
    p+=rl;
    GWEN_Buffer_AppendString(lbuf, "<br>\n");
  }

  GWEN_Buffer_AppendBuffer(keybuf, bbuf);
  GWEN_Buffer_free(bbuf);
  GWEN_Buffer_AppendString(lbuf, "</font>\n");
  GWEN_Buffer_AppendString(lbuf, "<br>\n");

  /* modulus */
  GWEN_Buffer_AppendString(lbuf, "\n");
  GWEN_Buffer_AppendString(lbuf, "<h4>");
  GWEN_Buffer_AppendString(lbuf, I18N("Modulus"));
  GWEN_Buffer_AppendString(lbuf, "</h4>\n");
  p=GWEN_Crypt_Token_KeyInfo_GetModulusData(ki);
  l=GWEN_Crypt_Token_KeyInfo_GetModulusLen(ki);
  if (!p || !l) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Bad key.");
    GWEN_Gui_ProgressLog(0, GWEN_LoggerLevel_Error,
                           I18N("Bad key"));
    return GWEN_ERROR_BAD_DATA;
  }

  GWEN_Buffer_AppendString(lbuf, "<font face=fixed>\n");
  bbuf=GWEN_Buffer_new(0, modLen+1, 0, 1);
  GWEN_Buffer_AppendBytes(bbuf, p, l);
  GWEN_Buffer_Rewind(bbuf);
  p=GWEN_Buffer_GetStart(bbuf);
  l=GWEN_Buffer_GetUsedBytes(bbuf);
  for (i=0; i<modLen; i+=16) {
    int rl=modLen-i;
    if (rl>16)
      rl=16;

    GWEN_Buffer_AppendString(lbuf, "  ");
    if (GWEN_Text_ToHexBuffer(p, rl, lbuf, 2, ' ', 0)) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Error converting to hex??");
      abort();
    }
    p+=rl;
    GWEN_Buffer_AppendString(lbuf, "<br>\n");
  }

  GWEN_Buffer_AppendBuffer(keybuf, bbuf);
  GWEN_Buffer_free(bbuf);
  GWEN_Buffer_AppendString(lbuf, "</font>\n");
  GWEN_Buffer_AppendString(lbuf, "<br>\n");

  GWEN_Buffer_AppendString(lbuf, "<h4>");
  GWEN_Buffer_AppendString(lbuf, I18N("Hash (RMD-160)"));
  GWEN_Buffer_AppendString(lbuf, "</h4>\n");
  GWEN_Buffer_AppendString(lbuf, "<font face=fixed>\n");
  rv=AH_Provider__HashRmd160((const uint8_t*)GWEN_Buffer_GetStart(keybuf),
			     GWEN_Buffer_GetUsedBytes(keybuf),
			     (uint8_t*)hashbuffer);
  if (rv) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Error hashing (%d)", rv);
    abort();
  }

  GWEN_Buffer_AppendString(lbuf, "  ");
  if (GWEN_Text_ToHexBuffer(hashbuffer, 20, lbuf, 2, ' ', 0)) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Error converting to hex??");
    abort();
  }
  GWEN_Buffer_AppendString(lbuf, "</font>\n");
  GWEN_Buffer_AppendString(lbuf, "<br>\n");

  GWEN_Buffer_AppendString(lbuf, "<h4>");
  GWEN_Buffer_AppendString(lbuf, I18N("Hash (SHA-256)"));
  GWEN_Buffer_AppendString(lbuf, "</h4>\n");
  GWEN_Buffer_AppendString(lbuf, "<font face=fixed>\n");
  rv=AH_Provider__HashSha256((const uint8_t*)GWEN_Buffer_GetStart(keybuf),
			     GWEN_Buffer_GetUsedBytes(keybuf),
			     (uint8_t*)hashbuffer);
  if (rv) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Error hashing (%d)", rv);
    abort();
  }
  GWEN_Buffer_free(keybuf);

  GWEN_Buffer_AppendString(lbuf, "  ");
  if (GWEN_Text_ToHexBuffer(hashbuffer, 32, lbuf, 2, ' ', 0)) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Error converting to hex??");
    abort();
  }
  GWEN_Buffer_AppendString(lbuf, "</font>\n");
  GWEN_Buffer_AppendString(lbuf, "<br>\n");


  if (!useBankKey) {
    GWEN_Buffer_AppendString(lbuf, "<br><br>\n");
    GWEN_Buffer_AppendString(lbuf,
                             I18N("I confirm that I created the above key "
                                  "for my electronic signature.\n"));
    GWEN_Buffer_AppendString(lbuf, "<br><br>\n");
    GWEN_Buffer_AppendString(lbuf, "<table>\n");
    GWEN_Buffer_AppendString(lbuf, "<tr><td>\n");
    GWEN_Buffer_AppendString(lbuf, "____________________________  ");
    GWEN_Buffer_AppendString(lbuf, "</td><td>\n");
    GWEN_Buffer_AppendString(lbuf, "____________________________  ");
    GWEN_Buffer_AppendString(lbuf, "</td></tr><tr><td>\n");
    GWEN_Buffer_AppendString(lbuf, I18N("Place, date"));
    GWEN_Buffer_AppendString(lbuf, "</td><td>\n");
    GWEN_Buffer_AppendString(lbuf, I18N("Signature"));
    GWEN_Buffer_AppendString(lbuf, "</td></tr></table>\n");
    GWEN_Buffer_AppendString(lbuf, "<br>\n");
  }

  return 0;
}



int AH_Provider_CreateKeys(AB_PROVIDER *pro,
			   AB_USER *u,
			   int nounmount) {
  GWEN_CRYPT_TOKEN *ct;
  const GWEN_CRYPT_TOKEN_CONTEXT *ctx;
  uint32_t keyId;
  GWEN_CRYPT_CRYPTALGO *algo;
  int rv;
  AH_HBCI *h;
  const GWEN_CRYPT_TOKEN_KEYINFO *oki;
  GWEN_CRYPT_TOKEN_KEYINFO *ki;
  int rdhType;
  int maxServerKeySizeInBits=0;

  h=AH_Provider_GetHbci(pro);
  assert(h);

  /* check crypt mode */
  if (AH_User_GetCryptMode(u)!=AH_CryptMode_Rdh) {
    DBG_ERROR(AQHBCI_LOGDOMAIN,
	      "Key generation not supported with this token");
    return GWEN_ERROR_INVALID;
  }

  rdhType=AH_User_GetRdhType(u);
  if (rdhType==0)
    rdhType=1;

  /* get token */
  rv=AB_Banking_GetCryptToken(AH_HBCI_GetBankingApi(h),
			      AH_User_GetTokenType(u),
			      AH_User_GetTokenName(u),
			      &ct);
  if (rv) {
    DBG_ERROR(AQHBCI_LOGDOMAIN,
	      "Error getting the user's crypt token (%d)", rv);
    return rv;
  }

  /* we always use 65537 as public exponent */
  GWEN_Crypt_Token_AddModes(ct, GWEN_CRYPT_TOKEN_MODE_EXP_65537);
  
  /* create algo */
  algo=GWEN_Crypt_CryptAlgo_new(GWEN_Crypt_CryptAlgoId_Rsa,
				GWEN_Crypt_CryptMode_None);

  /* open token for admin */
  if (!GWEN_Crypt_Token_IsOpen(ct)) {
    rv=GWEN_Crypt_Token_Open(ct, 1, 0);
    if (rv) {
      DBG_ERROR(AQHBCI_LOGDOMAIN,
		"Error opening crypt token (%d)", rv);
      GWEN_Crypt_CryptAlgo_free(algo);
      return rv;
    }
  }
  
  /* get context */
  ctx=GWEN_Crypt_Token_GetContext(ct, AH_User_GetTokenContextId(u), 0);
  if (ctx==NULL) {
    DBG_ERROR(AQHBCI_LOGDOMAIN,
	      "Could not get context %d", AH_User_GetTokenContextId(u));
    GWEN_Crypt_CryptAlgo_free(algo);
    return GWEN_ERROR_INVALID;
  }

  if (rdhType==10) {
    /* the specs say that for RDH-10 we must not create keys longer than the server's
     * sign key (or, if absent, the server's encipher key) */
    uint32_t skeyId;
    const GWEN_CRYPT_TOKEN_KEYINFO *ski;

    skeyId=GWEN_Crypt_Token_Context_GetVerifyKeyId(ctx);
    if (skeyId==0) {
      DBG_ERROR(AQHBCI_LOGDOMAIN,
		"No verify key id specified (internal error)");
      GWEN_Crypt_CryptAlgo_free(algo);
      return GWEN_ERROR_INVALID;
    }

    ski=GWEN_Crypt_Token_GetKeyInfo(ct, skeyId,
				    GWEN_CRYPT_TOKEN_KEYFLAGS_HASMODULUS,
				    0);
    if (ski==NULL) {
      skeyId=GWEN_Crypt_Token_Context_GetEncipherKeyId(ctx);
      if (skeyId==0) {
	DBG_ERROR(AQHBCI_LOGDOMAIN,
		  "No encipher key id specified (internal error)");
	GWEN_Crypt_CryptAlgo_free(algo);
	return GWEN_ERROR_INVALID;
      }
      ski=GWEN_Crypt_Token_GetKeyInfo(ct, skeyId,
				      GWEN_CRYPT_TOKEN_KEYFLAGS_HASMODULUS,
				      0);
    }

    if (ski) {
      const uint8_t *modPtr;
      uint32_t modLen;

      modPtr=GWEN_Crypt_Token_KeyInfo_GetModulusData(ski);
      modLen=GWEN_Crypt_Token_KeyInfo_GetModulusLen(ski);
      if (modPtr && modLen) {
	int nbits;

        nbits=modLen*8;
	while(modLen && *modPtr==0) {
	  nbits-=8;
	}
	if (modLen) {
	  int i;
	  uint8_t mask=0x80;
	  uint8_t b=*modPtr;

	  for (i=0; i<8; i++) {
	    if (b & mask)
	      break;
	    nbits--;
            mask>>=1;
	  }
	}
        if (nbits>1)
	  maxServerKeySizeInBits=nbits-1;
	else
	  maxServerKeySizeInBits=nbits;
      }
    }
  }

  switch(rdhType) {
  case 1:
    GWEN_Crypt_CryptAlgo_SetChunkSize(algo, 96);
    GWEN_Crypt_CryptAlgo_SetKeySizeInBits(algo, 768);
    break;
  case 2:
    GWEN_Crypt_CryptAlgo_SetChunkSize(algo, 256);
    GWEN_Crypt_CryptAlgo_SetKeySizeInBits(algo, 2048);
    break;
  case 3:
    GWEN_Crypt_CryptAlgo_SetChunkSize(algo, 256);
    GWEN_Crypt_CryptAlgo_SetKeySizeInBits(algo, 2048);
    break;
  case 5:
    GWEN_Crypt_CryptAlgo_SetChunkSize(algo, 256);
    GWEN_Crypt_CryptAlgo_SetKeySizeInBits(algo, 2048);
    break;
  case 10:
    if (maxServerKeySizeInBits) {
      int n=maxServerKeySizeInBits/8;
      if (maxServerKeySizeInBits%8)
	n++;
      GWEN_Crypt_CryptAlgo_SetChunkSize(algo, n);
      GWEN_Crypt_CryptAlgo_SetKeySizeInBits(algo, maxServerKeySizeInBits);
      /*DBG_ERROR(AQHBCI_LOGDOMAIN, "Creating keys of size: %d bytes, %d bits", n, maxServerKeySizeInBits);*/
    }
    else {
      GWEN_Crypt_CryptAlgo_SetChunkSize(algo, 512);
      GWEN_Crypt_CryptAlgo_SetKeySizeInBits(algo, 4096);
    }
    break;
  default:
    DBG_ERROR(AQHBCI_LOGDOMAIN, "RDH %d not supported", AH_User_GetRdhType(u));
    return GWEN_ERROR_INVALID;
  }

  GWEN_Gui_ProgressLog(0,
		       GWEN_LoggerLevel_Notice,
		       I18N("Creating keys, please wait..."));

  /* get cipher key id */
  keyId=GWEN_Crypt_Token_Context_GetDecipherKeyId(ctx);
  if (keyId==0) {
    DBG_ERROR(AQHBCI_LOGDOMAIN,
	      "No decipher key id specified (internal error)");
    GWEN_Crypt_CryptAlgo_free(algo);
    return GWEN_ERROR_INVALID;
  }
  
  /* generate cipher key */
  rv=GWEN_Crypt_Token_GenerateKey(ct, keyId, algo, 0);
  if (rv) {
    DBG_ERROR(AQHBCI_LOGDOMAIN,
	      "Error generating key (%d)", rv);
    GWEN_Gui_ProgressLog2(0,
			  GWEN_LoggerLevel_Error,
			  I18N("Error creating cipher key (%d)"), rv);
    GWEN_Crypt_CryptAlgo_free(algo);
    return rv;
  }

  /* set key number/version */
  oki=GWEN_Crypt_Token_GetKeyInfo(ct, keyId,
				  GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYVERSION |
				  GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYNUMBER,
				  0);
  if (oki==NULL) {
    DBG_ERROR(AQHBCI_LOGDOMAIN,
	      "Could not get KeyInfo for newly created key %d", keyId);
    GWEN_Crypt_CryptAlgo_free(algo);
    return rv;
  }
  ki=GWEN_Crypt_Token_KeyInfo_dup(oki);
  if (rdhType>1)
    GWEN_Crypt_Token_KeyInfo_SetKeyNumber(ki, rdhType);
  else
    GWEN_Crypt_Token_KeyInfo_SetKeyNumber(ki, 1);
  GWEN_Crypt_Token_KeyInfo_SetKeyVersion(ki, 1);
  GWEN_Crypt_Token_KeyInfo_AddFlags(ki,
				    GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYVERSION |
				    GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYNUMBER);
  rv=GWEN_Crypt_Token_SetKeyInfo(ct, keyId, ki, 0);
  GWEN_Crypt_Token_KeyInfo_free(ki);
  if (rv<0) {
    DBG_INFO(AQHBCI_LOGDOMAIN, "here (%d)", rv);
    GWEN_Crypt_CryptAlgo_free(algo);
    return rv;
  }
  
  /* get sign key id */
  keyId=GWEN_Crypt_Token_Context_GetSignKeyId(ctx);
  if (keyId==0) {
    DBG_ERROR(AQHBCI_LOGDOMAIN,
	      "No sign key id specified (internal error)");
    GWEN_Crypt_CryptAlgo_free(algo);
    return GWEN_ERROR_INVALID;
  }
  
  /* generate sign key */
  rv=GWEN_Crypt_Token_GenerateKey(ct, keyId, algo, 0);
  if (rv) {
    DBG_ERROR(AQHBCI_LOGDOMAIN,
	      "Error generating key (%d)", rv);
    GWEN_Gui_ProgressLog2(0,
			  GWEN_LoggerLevel_Error,
			  I18N("Error creating sign key (%d)"), rv);
    GWEN_Crypt_CryptAlgo_free(algo);
    return rv;
  }

  /* set key number/version */
  oki=GWEN_Crypt_Token_GetKeyInfo(ct, keyId,
				  GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYVERSION |
				  GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYNUMBER,
				  0);
  if (oki==NULL) {
    DBG_ERROR(AQHBCI_LOGDOMAIN,
	      "Could not get KeyInfo for newly created key %d", keyId);
    GWEN_Crypt_CryptAlgo_free(algo);
    return rv;
  }
  ki=GWEN_Crypt_Token_KeyInfo_dup(oki);
  if (rdhType>1)
    GWEN_Crypt_Token_KeyInfo_SetKeyNumber(ki, rdhType);
  else
    GWEN_Crypt_Token_KeyInfo_SetKeyNumber(ki, 2);
  GWEN_Crypt_Token_KeyInfo_SetKeyVersion(ki, 1);
  GWEN_Crypt_Token_KeyInfo_AddFlags(ki,
				    GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYVERSION |
				    GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYNUMBER);
  rv=GWEN_Crypt_Token_SetKeyInfo(ct, keyId, ki, 0);
  GWEN_Crypt_Token_KeyInfo_free(ki);
  if (rv<0) {
    DBG_INFO(AQHBCI_LOGDOMAIN, "here (%d)", rv);
    GWEN_Crypt_CryptAlgo_free(algo);
    return rv;
  }

  /* get auth sign key id */
  keyId=GWEN_Crypt_Token_Context_GetAuthSignKeyId(ctx);
  if (keyId) {
    /* generate auth sign key */
    rv=GWEN_Crypt_Token_GenerateKey(ct, keyId, algo, 0);
    if (rv) {
      DBG_ERROR(AQHBCI_LOGDOMAIN,
		"Error generating key (%d)", rv);
      GWEN_Gui_ProgressLog2(0,
			    GWEN_LoggerLevel_Error,
			    I18N("Error creating auth key (%d)"), rv);
      GWEN_Crypt_CryptAlgo_free(algo);
      return rv;
    }

    /* set key number/version */
    oki=GWEN_Crypt_Token_GetKeyInfo(ct, keyId,
				    GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYVERSION |
				    GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYNUMBER,
				    0);
    if (oki==NULL) {
      DBG_ERROR(AQHBCI_LOGDOMAIN,
		"Could not get KeyInfo for newly created key %d", keyId);
      GWEN_Crypt_CryptAlgo_free(algo);
      return rv;
    }
    ki=GWEN_Crypt_Token_KeyInfo_dup(oki);
    if (rdhType>1)
      GWEN_Crypt_Token_KeyInfo_SetKeyNumber(ki, rdhType);
    else
      GWEN_Crypt_Token_KeyInfo_SetKeyNumber(ki, 3);
    GWEN_Crypt_Token_KeyInfo_SetKeyVersion(ki, 1);
    GWEN_Crypt_Token_KeyInfo_AddFlags(ki,
				      GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYVERSION |
				      GWEN_CRYPT_TOKEN_KEYFLAGS_HASKEYNUMBER);
    rv=GWEN_Crypt_Token_SetKeyInfo(ct, keyId, ki, 0);
    GWEN_Crypt_Token_KeyInfo_free(ki);
    if (rv<0) {
      DBG_INFO(AQHBCI_LOGDOMAIN, "here (%d)", rv);
      GWEN_Crypt_CryptAlgo_free(algo);
      return rv;
    }
  }

  if (!nounmount) {
    /* close token */
    rv=GWEN_Crypt_Token_Close(ct, 0, 0);
    if (rv) {
      DBG_ERROR(AQHBCI_LOGDOMAIN,
		"Error closing crypt token (%d)", rv);
      GWEN_Crypt_CryptAlgo_free(algo);
      return rv;
    }
  }

  GWEN_Crypt_CryptAlgo_free(algo);
  return 0;
}



int AH_Provider_SendDtazv(AB_PROVIDER *pro,
			  AB_ACCOUNT *a,
			  AB_IMEXPORTER_CONTEXT *ctx,
			  const uint8_t *dataPtr,
			  uint32_t dataLen,
			  int withProgress, int nounmount, int doLock) {
  AH_PROVIDER *hp;
  AB_BANKING *ab;
  AH_HBCI *h;
  AB_USER *u;
  AH_JOB *job;
  AH_OUTBOX *ob;
  int rv;

  assert(pro);
  hp=GWEN_INHERIT_GETDATA(AB_PROVIDER, AH_PROVIDER, pro);
  assert(hp);

  assert(a);

  /* gather all objects */
  u=AB_Account_GetFirstUser(a);
  if (!u) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "No user for this account");
    return GWEN_ERROR_NOT_AVAILABLE;
  }

  ab=AB_Provider_GetBanking(pro);
  assert(ab);

  h=AH_Provider_GetHbci(pro);
  assert(h);

  job=AH_Job_ForeignTransferWH_new(u, a);
  if (!job) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Job not supported, should not happen");
    return GWEN_ERROR_GENERIC;
  }

  rv=AH_Job_ForeignTransferWH_SetDtazv(job, dataPtr, dataLen);
  if (rv) {
    DBG_INFO(AQHBCI_LOGDOMAIN, "here (%d)", rv);
    AH_Job_free(job);
    return rv;
  }

  AH_Job_AddSigner(job, AB_User_GetUserId(u));

  ob=AH_Outbox_new(h);
  AH_Outbox_AddJob(ob, job);

  rv=AH_Outbox_Execute(ob, ctx, withProgress, 1, doLock);
  if (rv) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Could not execute outbox.\n");
    if (!nounmount)
      AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
    AH_Job_free(job);
    AH_Outbox_free(ob);
    return rv;
  }

  AH_Outbox_free(ob);

  if (AH_Job_HasErrors(job) || AH_Job_GetStatus(job)==AH_JobStatusError) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Job has errors");
    // TODO: show errors
    AH_Job_free(job);
    if (!nounmount)
      AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));
    return GWEN_ERROR_GENERIC;
  }

  AH_Job_free(job);

  if (!nounmount)
    AB_Banking_ClearCryptTokenList(AH_HBCI_GetBankingApi(h));

  return 0;
}





#include "message_l.h"
#include <stdio.h>
#include <errno.h>


#if 0
static int AH_Provider_Test1(AB_PROVIDER *pro) {
  AB_BANKING *ab;
  FILE *f;

  ab=AB_Provider_GetBanking(pro);
  assert(ab);

  f=fopen("test.msg", "r");
  if (f) {
    GWEN_BUFFER *tbuf;
    AB_USER *u;
    AH_DIALOG *dlg;
    AH_MSG *msg;
    GWEN_DB_NODE *rsp;

    tbuf=GWEN_Buffer_new(0, 1024, 0, 1);
    while(!feof(f)) {
      char buffer[256];
      ssize_t i;

      i=fread(buffer, 1, sizeof(buffer), f);
      if (i<1) {
	DBG_ERROR(AQHBCI_LOGDOMAIN, "Error on read: %d (%s)",
		  errno, strerror(errno));
        return -1;
      }
      GWEN_Buffer_AppendBytes(tbuf, buffer, i);
    }
    fclose(f);
    GWEN_Buffer_Rewind(tbuf);

    u=AB_Banking_FindUser(ab, "aqhbci", "de", "28250110", "*", "*");
    assert(u);
    dlg=AH_Dialog_new(u, 0);
    assert(dlg);
    AH_Dialog_AddFlags(dlg, AH_DIALOG_FLAGS_INITIATOR);
    msg=AH_Msg_new(dlg);
    assert(msg);
    AH_Msg_SetBuffer(msg, tbuf);

    rsp=GWEN_DB_Group_new("response");
    if (AH_Msg_DecodeMsg(msg, rsp, GWEN_MSGENGINE_READ_FLAGS_DEFAULT)) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Could not decode this message:");
      AH_Msg_Dump(msg, stderr, 2);
      return -1;
    }

    fprintf(stderr, "Response is:\n");
    AH_Msg_Dump(msg, stderr, 2);
  }

  return 0;
}



static int AH_Provider_Test2(AB_PROVIDER *pro) {
  AB_BANKING *ab;
  AB_USER *u;
  AH_DIALOG *dlg;
  AH_MSG *msg;
  GWEN_BUFFER *tbuf;

  ab=AB_Provider_GetBanking(pro);
  assert(ab);

  tbuf=GWEN_Buffer_new(0, 1024, 0, 1);
  GWEN_Buffer_AppendString(tbuf, "(Test-String)");
  GWEN_Buffer_Rewind(tbuf);

  u=AB_Banking_FindUser(ab, "aqhbci", "de", "28250110", "*", "*");
  assert(u);
  dlg=AH_Dialog_new(u, 0);
  assert(dlg);
  AH_Dialog_AddFlags(dlg, AH_DIALOG_FLAGS_INITIATOR);
  msg=AH_Msg_new(dlg);
  assert(msg);
  AH_Msg_SetBuffer(msg, tbuf);

  AH_Msg_AddSignerId(msg, AB_User_GetUserId(u));
  AH_Msg_SetCrypterId(msg, AB_User_GetUserId(u));

  if (AH_Msg_EncodeMsg(msg)) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Could not decode this message:");
    AH_Msg_Dump(msg, stderr, 2);
    return -1;
  }

  fprintf(stderr, "Message is:\n");
  AH_Msg_Dump(msg, stderr, 2);

  return 0;
}



static int AH_Provider_Test3(AB_PROVIDER *pro) {
  AB_BANKING *ab;
  FILE *f;

  ab=AB_Provider_GetBanking(pro);
  assert(ab);

  f=fopen("test.msg", "r");
  if (f) {
    GWEN_BUFFER *tbuf;
    AB_USER *u;
    AH_DIALOG *dlg;
    AH_MSG *msg;
    int rv;
    GWEN_DB_NODE *rsp;

    tbuf=GWEN_Buffer_new(0, 1024, 0, 1);
    while(!feof(f)) {
      char buffer[256];
      ssize_t i;

      i=fread(buffer, 1, sizeof(buffer), f);
      if (i<1) {
	DBG_ERROR(AQHBCI_LOGDOMAIN, "Error on read: %d (%s)",
		  errno, strerror(errno));
        return -1;
      }
      GWEN_Buffer_AppendBytes(tbuf, buffer, i);
    }
    fclose(f);
    GWEN_Buffer_Rewind(tbuf);

    u=AB_Banking_FindUser(ab, "aqhbci", "de", "20090500", "*", "*");
    assert(u);
    dlg=AH_Dialog_new(u, 0);
    assert(dlg);
    AH_Dialog_AddFlags(dlg, AH_DIALOG_FLAGS_INITIATOR);
    msg=AH_Msg_new(dlg);
    assert(msg);
    AH_Msg_SetBuffer(msg, tbuf);

    rsp=GWEN_DB_Group_new("response");
    rv=AH_Msg_DecodeMsg(msg, rsp, GWEN_MSGENGINE_READ_FLAGS_DEFAULT);
    if (rv<0) {
      DBG_ERROR(AQHBCI_LOGDOMAIN, "Could not decode this message: (%d)", rv);
      AH_Msg_Dump(msg, stderr, 2);
      return -1;
    }

    fprintf(stderr, "Response is:\n");
    AH_Msg_Dump(msg, stderr, 2);
  }

  return 0;
}
#endif



static int AH_Provider_Test4(AB_PROVIDER *pro) {
  AB_BANKING *ab;
  AB_USER *u;
  AH_DIALOG *dlg;
  AH_MSG *msg;
  GWEN_BUFFER *tbuf;

  ab=AB_Provider_GetBanking(pro);
  assert(ab);

  tbuf=GWEN_Buffer_new(0, 1024, 0, 1);
  GWEN_Buffer_AppendString(tbuf, "(Test-String)");
  GWEN_Buffer_Rewind(tbuf);

  u=AB_Banking_FindUser(ab, "aqhbci", "de", "20090500", "*", "*");
  assert(u);
  dlg=AH_Dialog_new(u);
  assert(dlg);
  AH_Dialog_AddFlags(dlg, AH_DIALOG_FLAGS_INITIATOR);
  msg=AH_Msg_new(dlg);
  assert(msg);
  AH_Msg_SetBuffer(msg, tbuf);
  AH_Msg_SetHbciVersion(msg, 220);

  AH_Msg_AddSignerId(msg, AB_User_GetUserId(u));
  AH_Msg_SetCrypterId(msg, AB_User_GetUserId(u));

  if (AH_Msg_EncodeMsg(msg)) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "Could not decode this message:");
    AH_Msg_Dump(msg, stderr, 2);
    return -1;
  }

  fprintf(stderr, "Message is:\n");
  AH_Msg_Dump(msg, stderr, 2);

  return 0;
}



int AH_Provider_Test(AB_PROVIDER *pro) {
  return AH_Provider_Test4(pro);
}




int AH_Provider_ValidateTransfer(AB_TRANSACTION *t,
				 AB_JOB *j,
				 const AB_TRANSACTION_LIMITS *lim) {
  const GWEN_STRINGLIST *sl;
  int maxn;
  int maxs;
  int n;
  const char *s;

  /* check purpose */
  if (lim) {
    maxn=AB_TransactionLimits_GetMaxLinesPurpose(lim);
    maxs=AB_TransactionLimits_GetMaxLenPurpose(lim);
  }
  else {
    DBG_INFO(AQHBCI_LOGDOMAIN, "No transaction limits");
    maxn=0;
    maxs=0;
  }
  sl=AB_Transaction_GetPurpose(t);
  n=0;
  if (sl) {
    GWEN_STRINGLISTENTRY *se;
    GWEN_STRINGLIST *nsl;
    const char *p;

    nsl=GWEN_StringList_new();
    se=GWEN_StringList_FirstEntry(sl);
    while(se) {
      p=GWEN_StringListEntry_Data(se);
      if (p && *p) {
	char *np;
	int l;
	GWEN_BUFFER *tbuf;

	n++;
	if (maxn && n>maxn) {
	  DBG_ERROR(AQHBCI_LOGDOMAIN,
		    "Too many purpose lines (%d>%d)", n, maxn);
	  GWEN_StringList_free(nsl);
	  return GWEN_ERROR_INVALID;
	}
	tbuf=GWEN_Buffer_new(0, maxs, 0, 1);
	AB_ImExporter_Utf8ToDta(p, -1, tbuf);
	GWEN_Text_CondenseBuffer(tbuf);
	l=GWEN_Buffer_GetUsedBytes(tbuf);
	if (maxs && l>maxs) {
	  DBG_ERROR(AQHBCI_LOGDOMAIN,
		    "Too many chars in purpose line %d (%d>%d)", n, l, maxs);
	  GWEN_StringList_free(nsl);
	  return GWEN_ERROR_INVALID;
	}
	np=(char*)GWEN_Memory_malloc(l+1);
	memmove(np, GWEN_Buffer_GetStart(tbuf), l+1);
	GWEN_Buffer_free(tbuf);
	/* let string list take the newly alllocated string */
	GWEN_StringList_AppendString(nsl, np, 1, 0);
      }
      se=GWEN_StringListEntry_Next(se);
    } /* while */
    AB_Transaction_SetPurpose(t, nsl);
  }
  if (!n) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "No purpose lines");
    return GWEN_ERROR_INVALID;
  }

  /* check remote name */
  if (lim) {
    maxn=AB_TransactionLimits_GetMaxLinesRemoteName(lim);
    maxs=AB_TransactionLimits_GetMaxLenRemoteName(lim);
  }
  else {
    maxn=0;
    maxs=0;
  }
  sl=AB_Transaction_GetRemoteName(t);
  n=0;
  if (sl) {
    GWEN_STRINGLISTENTRY *se;
    GWEN_STRINGLIST *nsl;
    const char *p;

    nsl=GWEN_StringList_new();
    se=GWEN_StringList_FirstEntry(sl);
    while(se) {
      p=GWEN_StringListEntry_Data(se);
      if (p && *p) {
	char *np;
	int l;
        GWEN_BUFFER *tbuf;

	n++;
	if (maxn && n>maxn) {
	  DBG_ERROR(AQHBCI_LOGDOMAIN,
		    "Too many remote name lines (%d>%d)",
		    n, maxn);
          GWEN_StringList_free(nsl);
	  return GWEN_ERROR_INVALID;
	}
	tbuf=GWEN_Buffer_new(0, maxs, 0, 1);
        AB_ImExporter_Utf8ToDta(p, -1, tbuf);
	GWEN_Text_CondenseBuffer(tbuf);
	l=GWEN_Buffer_GetUsedBytes(tbuf);
	if (l>maxs) {
	  DBG_ERROR(AQHBCI_LOGDOMAIN,
		   "Too many chars in remote name line %d (%d>%d)",
		   n, l, maxs);
          GWEN_StringList_free(nsl);
	  return GWEN_ERROR_INVALID;
	}
	np=(char*)GWEN_Memory_malloc(l+1);
	memmove(np, GWEN_Buffer_GetStart(tbuf), l+1);
	GWEN_Buffer_free(tbuf);
	/* let string list take the newly alllocated string */
	GWEN_StringList_AppendString(nsl, np, 1, 0);
      }
      se=GWEN_StringListEntry_Next(se);
    } /* while */
    AB_Transaction_SetRemoteName(t, nsl);
  }
  if (!n) {
    DBG_ERROR(AQHBCI_LOGDOMAIN, "No remote name lines");
    return GWEN_ERROR_INVALID;
  }

  /* check local name */
  s=AB_Transaction_GetLocalName(t);
  if (!s) {
    AB_ACCOUNT *a;

    DBG_WARN(AQHBCI_LOGDOMAIN,
	     "No local name, filling in");
    a=AB_Job_GetAccount(j);
    assert(a);
    s=AB_Account_GetOwnerName(a);
    if (!s) {
      DBG_ERROR(AQHBCI_LOGDOMAIN,
		"No owner name in account, giving up");
      return GWEN_ERROR_INVALID;
    }
    AB_Transaction_SetLocalName(t, s);
  }

  /* check local bank code */
  s=AB_Transaction_GetLocalBankCode(t);
  if (!s) {
    AB_ACCOUNT *a;

    DBG_WARN(AQHBCI_LOGDOMAIN,
	     "No local bank code, filling in");
    a=AB_Job_GetAccount(j);
    assert(a);
    s=AB_Account_GetBankCode(a);
    assert(s);
    AB_Transaction_SetLocalBankCode(t, s);
  }

  /* check local account number */
  s=AB_Transaction_GetLocalAccountNumber(t);
  if (!s) {
    AB_ACCOUNT *a;

    DBG_WARN(AQHBCI_LOGDOMAIN,
	     "No local account number, filling in");
    a=AB_Job_GetAccount(j);
    assert(a);
    s=AB_Account_GetAccountNumber(a);
    assert(s);
    AB_Transaction_SetLocalAccountNumber(t, s);
  }

  /* check local account suffix */
  s=AB_Transaction_GetLocalSuffix(t);
  if (!s) {
    AB_ACCOUNT *a;

    DBG_INFO(AQHBCI_LOGDOMAIN,
	     "No local suffix, filling in (if possible)");
    a=AB_Job_GetAccount(j);
    assert(a);
    s=AH_Account_GetSuffix(a);
    if (s)
      AB_Transaction_SetLocalSuffix(t, s);
  }


 /* check text key */
  if (lim) {
    if (GWEN_StringList_Count(AB_TransactionLimits_GetValuesTextKey(lim))){
      char numbuf[32];

      n=AB_Transaction_GetTextKey(t);
      if (n==0) {
        switch(AB_Job_GetType(j)) {
        case AB_Job_TypeDebitNote:
        case AB_Job_TypeSepaDebitNote:
          n=5;  /* "Lastschrift" */
          break;
        case AB_Job_TypeTransfer:
        case AB_Job_TypeCreateStandingOrder:
        case AB_Job_TypeModifyStandingOrder:
        case AB_Job_TypeDeleteStandingOrder:
        case AB_Job_TypeInternalTransfer:
        case AB_Job_TypeSepaTransfer:
        default:
          n=51; /* "Ueberweisung" */
          break;
        }
        AB_Transaction_SetTextKey(t, n);
      }

      snprintf(numbuf, sizeof(numbuf), "%d", n);
      if (!AB_TransactionLimits_HasValuesTextKey(lim, numbuf)) {
	DBG_ERROR(AQHBCI_LOGDOMAIN, "Text key \"%s\" not supported by bank",
		  numbuf);
	return GWEN_ERROR_INVALID;
      }
    }
  }

  if (lim) {
    const GWEN_TIME *ti;

    switch(AB_Job_GetType(j)) {
    case AB_Job_TypeCreateStandingOrder:
    case AB_Job_TypeModifyStandingOrder:
    case AB_Job_TypeDeleteStandingOrder:
      /* additional checks for standing orders */

      /* check period */
      if (AB_Transaction_GetPeriod(t)==AB_Transaction_PeriodMonthly) {
        const GWEN_STRINGLIST *sl;

        /* check cycle */
        sl=AB_TransactionLimits_GetValuesCycleMonth(lim);
        if (GWEN_StringList_Count(sl)){
          char numbuf[32];

          n=AB_Transaction_GetCycle(t);
          if (n==0) {
            DBG_ERROR(AQHBCI_LOGDOMAIN,
                      "No cycle given");
            return GWEN_ERROR_INVALID;
          }

          snprintf(numbuf, sizeof(numbuf), "%d", n);
          if (!AB_TransactionLimits_HasValuesCycleMonth(lim, numbuf) &&
              !AB_TransactionLimits_HasValuesCycleMonth(lim, "0")) {
            DBG_ERROR(AQHBCI_LOGDOMAIN,
                      "Month day \"%s\" not supported by bank",
                      numbuf);
            return GWEN_ERROR_INVALID;
          }
        }

        /* check execution day */
        sl=AB_TransactionLimits_GetValuesExecutionDayMonth(lim);
        if (GWEN_StringList_Count(sl)){
          char numbuf[32];

          n=AB_Transaction_GetExecutionDay(t);
          if (n==0) {
            DBG_ERROR(AQHBCI_LOGDOMAIN,
                      "No execution day given");
            return GWEN_ERROR_INVALID;
          }

          snprintf(numbuf, sizeof(numbuf), "%d", n);
          if (!AB_TransactionLimits_HasValuesExecutionDayMonth(lim, numbuf) &&
              !AB_TransactionLimits_HasValuesExecutionDayMonth(lim, "0")) {
            DBG_ERROR(AQHBCI_LOGDOMAIN,
                      "Execution month day \"%s\" not supported by bank",
                      numbuf);
            return GWEN_ERROR_INVALID;
          }
        } /* if there are limits */
      }
      else if (AB_Transaction_GetPeriod(t)==AB_Transaction_PeriodWeekly) {
        const GWEN_STRINGLIST *sl;

        /* check cycle */
        sl=AB_TransactionLimits_GetValuesCycleWeek(lim);
        if (GWEN_StringList_Count(sl)) {
          char numbuf[32];

          n=AB_Transaction_GetCycle(t);
          if (n==0) {
            DBG_ERROR(AQHBCI_LOGDOMAIN,
                      "No cycle given");
            return GWEN_ERROR_INVALID;
          }

          snprintf(numbuf, sizeof(numbuf), "%d", n);
          if (!AB_TransactionLimits_HasValuesCycleWeek(lim, numbuf) &&
              !AB_TransactionLimits_HasValuesCycleWeek(lim, "0")) {
            DBG_ERROR(AQHBCI_LOGDOMAIN,
                      "Week day \"%s\" not supported by bank",
                      numbuf);
            return GWEN_ERROR_INVALID;
          }
        } /* if there are limits */

        /* check execution day */
        sl=AB_TransactionLimits_GetValuesExecutionDayWeek(lim);
        if (GWEN_StringList_Count(sl)){
          char numbuf[32];

          n=AB_Transaction_GetExecutionDay(t);
          if (n==0) {
            DBG_ERROR(AQHBCI_LOGDOMAIN,
                      "No execution day given");
            return GWEN_ERROR_INVALID;
          }

          snprintf(numbuf, sizeof(numbuf), "%d", n);
          if (!AB_TransactionLimits_HasValuesExecutionDayWeek(lim, numbuf) &&
              !AB_TransactionLimits_HasValuesExecutionDayWeek(lim, "0")) {
            DBG_ERROR(AQHBCI_LOGDOMAIN,
                      "Execution month day \"%s\" not supported by bank",
                      numbuf);
            return GWEN_ERROR_INVALID;
          }
        } /* if there are limits */
      }
      else {
        DBG_ERROR(AQHBCI_LOGDOMAIN,
                  "Unsupported period %d", AB_Transaction_GetPeriod(t));
        return GWEN_ERROR_INVALID;
      }

      /* check setup times */
      ti=AB_Transaction_GetFirstExecutionDate(t);
      if (ti && AB_Job_GetType(j)==AB_Job_TypeCreateStandingOrder) {
	GWEN_TIME *currDate;
        int dt;

        currDate=GWEN_CurrentTime();
        assert(currDate);
        dt=((int)GWEN_Time_DiffSeconds(ti, currDate))/(60*60*24);
        GWEN_Time_free(currDate);


        /* check minimum setup time */
        n=AB_TransactionLimits_GetMinValueSetupTime(lim);
        if (n && dt<n) {
          DBG_ERROR(AQHBCI_LOGDOMAIN,
                    "Minimum setup time violated");
          return GWEN_ERROR_INVALID;
        }

        /* check maximum setup time */
        n=AB_TransactionLimits_GetMaxValueSetupTime(lim);
        if (n && dt>n) {
          DBG_ERROR(AQHBCI_LOGDOMAIN,
                    "Maximum setup time violated");
          return GWEN_ERROR_INVALID;
        }
      }
      break;

    case AB_Job_TypeCreateDatedTransfer:
    case AB_Job_TypeModifyDatedTransfer:
      /* check setup times */
      ti=AB_Transaction_GetDate(t);
      if (ti) {
        GWEN_TIME *currDate;
        int dt;

        currDate=GWEN_CurrentTime();
        assert(currDate);
        dt=((int)GWEN_Time_DiffSeconds(ti, currDate))/(60*60*24);
        GWEN_Time_free(currDate);

        /* check minimum setup time */
        n=AB_TransactionLimits_GetMinValueSetupTime(lim);
        if (n && dt<n) {
          DBG_ERROR(AQHBCI_LOGDOMAIN,
                    "Minimum setup time violated");
          return GWEN_ERROR_INVALID;
        }

        /* check maximum setup time */
        n=AB_TransactionLimits_GetMaxValueSetupTime(lim);
        if (n && dt>n) {
          DBG_ERROR(AQHBCI_LOGDOMAIN,
                    "Maximum setup time violated");
          return GWEN_ERROR_INVALID;
        }
      }
      break;

    case AB_Job_TypeDeleteDatedTransfer:
      break;

      /* --------------- add more jobs here ---------------------- */

    default:
      break;
    } /* switch */
  }
  return 0;
}




