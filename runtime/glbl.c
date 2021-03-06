/* glbl.c - this module holds global defintions and data items.
 * These are shared among the runtime library. Their use should be
 * limited to cases where it is actually needed. The main intension for
 * implementing them was support for the transistion from v2 to v4
 * (with fully modular design), but it turned out that there may also
 * be some other good use cases besides backwards-compatibility.
 *
 * Module begun 2008-04-16 by Rainer Gerhards
 *
 * Copyright 2008-2014 Rainer Gerhards and Adiscon GmbH.
 *
 * This file is part of the rsyslog runtime library.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *       http://www.apache.org/licenses/LICENSE-2.0
 *       -or-
 *       see COPYING.ASL20 in the source distribution
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "config.h"
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <assert.h>

#include "rsyslog.h"
#include "obj.h"
#include "unicode-helper.h"
#include "cfsysline.h"
#include "glbl.h"
#include "prop.h"
#include "atomic.h"
#include "errmsg.h"
#include "action.h"
#include "rainerscript.h"
#include "net.h"

/* some defaults */
#ifndef DFLT_NETSTRM_DRVR
#	define DFLT_NETSTRM_DRVR ((uchar*)"ptcp")
#endif

/* static data */
DEFobjStaticHelpers
DEFobjCurrIf(prop)
DEFobjCurrIf(errmsg)
DEFobjCurrIf(net)

/* static data
 * For this object, these variables are obviously what makes the "meat" of the
 * class...
 */
int glblDebugOnShutdown = 0;	/* start debug log when we are shut down */

static struct cnfobj *mainqCnfObj = NULL;/* main queue object, to be used later in startup sequence */
int bProcessInternalMessages = 1;	/* Should rsyslog itself process internal messages?
					 * 1 - yes
					 * 0 - send them to libstdlog (e.g. to push to journal)
					 */
static uchar *pszWorkDir = NULL;
static int bOptimizeUniProc = 1;	/* enable uniprocessor optimizations */
static int bParseHOSTNAMEandTAG = 1;	/* parser modification (based on startup params!) */
static int bPreserveFQDN = 0;		/* should FQDNs always be preserved? */
static int iMaxLine = 8096;		/* maximum length of a syslog message */
static int iDefPFFamily = PF_UNSPEC;     /* protocol family (IPv4, IPv6 or both) */
static int bDropMalPTRMsgs = 0;/* Drop messages which have malicious PTR records during DNS lookup */
static int option_DisallowWarning = 1;	/* complain if message from disallowed sender is received */
static int bDisableDNS = 0; /* don't look up IP addresses of remote messages */
static prop_t *propLocalIPIF = NULL;/* IP address to report for the local host (default is 127.0.0.1) */
static prop_t *propLocalHostName = NULL;/* our hostname as FQDN - read-only after startup */
static prop_t *propLocalHostNameToDelete = NULL;/* see GenerateLocalHostName function hdr comment! */
static uchar *LocalHostName = NULL;/* our hostname  - read-only after startup, except HUP */
static uchar *LocalHostNameOverride = NULL;/* user-overridden hostname - read-only after startup */
static uchar *LocalFQDNName = NULL;/* our hostname as FQDN - read-only after startup, except HUP */
static uchar *LocalDomain = NULL;/* our local domain name  - read-only after startup, except HUP */
static char **StripDomains = NULL;/* these domains may be stripped before writing logs  - r/o after s.u., never touched by init */
static char **LocalHosts = NULL;/* these hosts are logged with their hostname  - read-only after startup, never touched by init */
static uchar *pszDfltNetstrmDrvr = NULL; /* module name of default netstream driver */
static uchar *pszDfltNetstrmDrvrCAF = NULL; /* default CA file for the netstrm driver */
static uchar *pszDfltNetstrmDrvrKeyFile = NULL; /* default key file for the netstrm driver (server) */
static uchar *pszDfltNetstrmDrvrCertFile = NULL; /* default cert file for the netstrm driver (server) */
static int bTerminateInputs = 0;		/* global switch that inputs shall terminate ASAP (1=> terminate) */
static uchar cCCEscapeChar = '#'; /* character to be used to start an escape sequence for control chars */
static int bDropTrailingLF = 1; /* drop trailing LF's on reception? */
static int bEscapeCCOnRcv = 1; /* escape control characters on reception: 0 - no, 1 - yes */
static int bSpaceLFOnRcv = 0; /* replace newlines with spaces on reception: 0 - no, 1 - yes */
static int bEscape8BitChars = 0; /* escape characters > 127 on reception: 0 - no, 1 - yes */
static int bEscapeTab = 1; /* escape tab control character when doing CC escapes: 0 - no, 1 - yes */
static int bParserEscapeCCCStyle = 0; /* escape control characters in c style: 0 - no, 1 - yes */

pid_t glbl_ourpid;
#ifndef HAVE_ATOMIC_BUILTINS
static DEF_ATOMIC_HELPER_MUT(mutTerminateInputs);
#endif
#ifdef USE_UNLIMITED_SELECT
static int iFdSetSize = howmany(FD_SETSIZE, __NFDBITS) * sizeof (fd_mask); /* size of select() bitmask in bytes */
#endif
static uchar *SourceIPofLocalClient = NULL;	/* [ar] Source IP for local client to be used on multihomed host */


/* tables for interfacing with the v6 config system */
static struct cnfparamdescr cnfparamdescr[] = {
	{ "workdirectory", eCmdHdlrString, 0 },
	{ "dropmsgswithmaliciousdnsptrrecords", eCmdHdlrBinary, 0 },
	{ "localhostname", eCmdHdlrGetWord, 0 },
	{ "preservefqdn", eCmdHdlrBinary, 0 },
	{ "debug.onshutdown", eCmdHdlrBinary, 0 },
	{ "debug.logfile", eCmdHdlrString, 0 },
	{ "defaultnetstreamdrivercafile", eCmdHdlrString, 0 },
	{ "defaultnetstreamdriverkeyfile", eCmdHdlrString, 0 },
	{ "defaultnetstreamdriver", eCmdHdlrString, 0 },
	{ "maxmessagesize", eCmdHdlrSize, 0 },
	{ "action.reportsuspension", eCmdHdlrBinary, 0 },
	{ "parser.controlcharacterescapeprefix", eCmdHdlrGetChar, 0 },
	{ "parser.droptrailinglfonreception", eCmdHdlrBinary, 0 },
	{ "parser.escapecontrolcharactersonreceive", eCmdHdlrBinary, 0 },
	{ "parser.spacelfonreceive", eCmdHdlrBinary, 0 },
	{ "parser.escape8bitcharactersonreceive", eCmdHdlrBinary, 0},
	{ "parser.escapecontrolcharactertab", eCmdHdlrBinary, 0},
	{ "parser.escapecontrolcharacterscstyle", eCmdHdlrBinary, 0 },
	{ "processinternalmessages", eCmdHdlrBinary, 0 }
};
static struct cnfparamblk paramblk =
	{ CNFPARAMBLK_VERSION,
	  sizeof(cnfparamdescr)/sizeof(struct cnfparamdescr),
	  cnfparamdescr
	};

static struct cnfparamvals *cnfparamvals = NULL;
/* we need to support multiple calls into our param block, so we need
 * to persist the current settings. Note that this must be re-set
 * each time a new config load begins (TODO: create interface?)
 */

/* define a macro for the simple properties' set and get functions
 * (which are always the same). This is only suitable for pretty
 * simple cases which require neither checks nor memory allocation.
 */
#define SIMP_PROP(nameFunc, nameVar, dataType) \
	SIMP_PROP_GET(nameFunc, nameVar, dataType) \
	SIMP_PROP_SET(nameFunc, nameVar, dataType) 
#define SIMP_PROP_SET(nameFunc, nameVar, dataType) \
static rsRetVal Set##nameFunc(dataType newVal) \
{ \
	nameVar = newVal; \
	return RS_RET_OK; \
} 
#define SIMP_PROP_GET(nameFunc, nameVar, dataType) \
static dataType Get##nameFunc(void) \
{ \
	return(nameVar); \
}

SIMP_PROP(ParseHOSTNAMEandTAG, bParseHOSTNAMEandTAG, int)
SIMP_PROP(OptimizeUniProc, bOptimizeUniProc, int)
SIMP_PROP(PreserveFQDN, bPreserveFQDN, int)
SIMP_PROP(mainqCnfObj, mainqCnfObj, struct cnfobj *)
SIMP_PROP(MaxLine, iMaxLine, int)
SIMP_PROP(DefPFFamily, iDefPFFamily, int) /* note that in the future we may check the family argument */
SIMP_PROP(DropMalPTRMsgs, bDropMalPTRMsgs, int)
SIMP_PROP(Option_DisallowWarning, option_DisallowWarning, int)
SIMP_PROP(DisableDNS, bDisableDNS, int)
SIMP_PROP(StripDomains, StripDomains, char**)
SIMP_PROP(LocalHosts, LocalHosts, char**)
SIMP_PROP(ParserControlCharacterEscapePrefix, cCCEscapeChar, uchar)
SIMP_PROP(ParserDropTrailingLFOnReception, bDropTrailingLF, int)
SIMP_PROP(ParserEscapeControlCharactersOnReceive, bEscapeCCOnRcv, int)
SIMP_PROP(ParserSpaceLFOnReceive, bSpaceLFOnRcv, int)
SIMP_PROP(ParserEscape8BitCharactersOnReceive, bEscape8BitChars, int)
SIMP_PROP(ParserEscapeControlCharacterTab, bEscapeTab, int)
SIMP_PROP(ParserEscapeControlCharactersCStyle, bParserEscapeCCCStyle, int)
#ifdef USE_UNLIMITED_SELECT
SIMP_PROP(FdSetSize, iFdSetSize, int)
#endif

SIMP_PROP_SET(DfltNetstrmDrvr, pszDfltNetstrmDrvr, uchar*) /* TODO: use custom function which frees existing value */
SIMP_PROP_SET(DfltNetstrmDrvrCAF, pszDfltNetstrmDrvrCAF, uchar*) /* TODO: use custom function which frees existing value */
SIMP_PROP_SET(DfltNetstrmDrvrKeyFile, pszDfltNetstrmDrvrKeyFile, uchar*) /* TODO: use custom function which frees existing value */
SIMP_PROP_SET(DfltNetstrmDrvrCertFile, pszDfltNetstrmDrvrCertFile, uchar*) /* TODO: use custom function which frees existing value */

#undef SIMP_PROP
#undef SIMP_PROP_SET
#undef SIMP_PROP_GET


/* return global input termination status
 * rgerhards, 2009-07-20
 */
static int GetGlobalInputTermState(void)
{
	return ATOMIC_FETCH_32BIT(&bTerminateInputs, &mutTerminateInputs);
}


/* set global termination state to "terminate". Note that this is a
 * "once in a lifetime" action which can not be undone. -- gerhards, 2009-07-20
 */
static void SetGlobalInputTermination(void)
{
	ATOMIC_STORE_1_TO_INT(&bTerminateInputs, &mutTerminateInputs);
}


/* set the local host IP address to a specific string. Helper to
 * small set of functions. No checks done, caller must ensure it is
 * ok to call. Most importantly, the IP address must not already have 
 * been set. -- rgerhards, 2012-03-21
 */
static inline rsRetVal
storeLocalHostIPIF(uchar *myIP)
{
	DEFiRet;
	CHKiRet(prop.Construct(&propLocalIPIF));
	CHKiRet(prop.SetString(propLocalIPIF, myIP, ustrlen(myIP)));
	CHKiRet(prop.ConstructFinalize(propLocalIPIF));
	DBGPRINTF("rsyslog/glbl: using '%s' as localhost IP\n", myIP);
finalize_it:
	RETiRet;
}


/* This function is used to set the IP address that is to be
 * reported for the local host. Note that in order to ease things
 * for the v6 config interface, we do not allow to set this more
 * than once.
 * rgerhards, 2012-03-21
 */
static rsRetVal
setLocalHostIPIF(void __attribute__((unused)) *pVal, uchar *pNewVal)
{
	uchar myIP[128];
	rsRetVal localRet;
	DEFiRet;

	CHKiRet(objUse(net, CORE_COMPONENT));

	if(propLocalIPIF != NULL) {
		errmsg.LogError(0, RS_RET_ERR, "$LocalHostIPIF is already set "
				"and cannot be reset; place it at TOP OF rsyslog.conf!");
		ABORT_FINALIZE(RS_RET_ERR);
	}

	localRet = net.GetIFIPAddr(pNewVal, AF_UNSPEC, myIP, (int) sizeof(myIP));
	if(localRet != RS_RET_OK) {
		errmsg.LogError(0, RS_RET_ERR, "$LocalHostIPIF: IP address for interface "
				"'%s' cannnot be obtained - ignoring directive", pNewVal);
	} else  {
		storeLocalHostIPIF(myIP);
	}


finalize_it:
	free(pNewVal); /* no longer needed -> is in prop! */
	RETiRet;
}


/* This function is used to set the global work directory name. 
 * It verifies that the provided directory actually exists and
 * emits an error message if not.
 * rgerhards, 2011-02-16
 */
static rsRetVal setWorkDir(void __attribute__((unused)) *pVal, uchar *pNewVal)
{
	size_t lenDir;
	int i;
	struct stat sb;
	DEFiRet;

	/* remove trailing slashes */
	lenDir = ustrlen(pNewVal);
	i = lenDir - 1;
	while(i > 0 && pNewVal[i] == '/') {
		--i;
	}

	if(i < 0) {
		errmsg.LogError(0, RS_RET_ERR_WRKDIR, "$WorkDirectory: empty value "
				"- directive ignored");
		ABORT_FINALIZE(RS_RET_ERR_WRKDIR);
	}

	if(i != (int) lenDir - 1) {
		pNewVal[i+1] = '\0';
		errmsg.LogError(0, RS_RET_WRN_WRKDIR, "$WorkDirectory: trailing slashes "
			"removed, new value is '%s'", pNewVal);
	}

	if(stat((char*) pNewVal, &sb) != 0) {
		errmsg.LogError(0, RS_RET_ERR_WRKDIR, "$WorkDirectory: %s can not be "
				"accessed, probably does not exist - directive ignored", pNewVal);
		ABORT_FINALIZE(RS_RET_ERR_WRKDIR);
	}

	if(!S_ISDIR(sb.st_mode)) {
		errmsg.LogError(0, RS_RET_ERR_WRKDIR, "$WorkDirectory: %s not a directory - directive ignored", 
				pNewVal);
		ABORT_FINALIZE(RS_RET_ERR_WRKDIR);
	}

	free(pszWorkDir);
	pszWorkDir = pNewVal;

finalize_it:
	RETiRet;
}


static rsRetVal
setDebugFile(void __attribute__((unused)) *pVal, uchar *pNewVal)
{
	DEFiRet;
	dbgSetDebugFile(pNewVal);
	free(pNewVal);
	RETiRet;
}


static rsRetVal
setDebugLevel(void __attribute__((unused)) *pVal, int level)
{
	DEFiRet;
	dbgSetDebugLevel(level);
	dbgprintf("debug level %d set via config file\n", level);
	dbgprintf("This is rsyslog version " VERSION "\n");
	RETiRet;
}


/* return our local IP.
 * If no local IP is set, "127.0.0.1" is selected *and* set. This
 * is an intensional side effect that we do in order to keep things
 * consistent and avoid config errors (this will make us not accept
 * setting the local IP address once a module has obtained it - so
 * it forces the $LocalHostIPIF directive high up in rsyslog.conf)
 * rgerhards, 2012-03-21
 */
static prop_t*
GetLocalHostIP(void)
{
	if(propLocalIPIF == NULL)
		storeLocalHostIPIF((uchar*)"127.0.0.1");
	return(propLocalIPIF);
}


/* set our local hostname. Free previous hostname, if it was already set.
 * Note that we do now do this in a thread
 * "once in a lifetime" action which can not be undone. -- gerhards, 2009-07-20
 */
static rsRetVal
SetLocalHostName(uchar *newname)
{
	free(LocalHostName);
	LocalHostName = newname;
	return RS_RET_OK;
}


/* return our local hostname. if it is not set, "[localhost]" is returned
 */
static uchar*
GetLocalHostName(void)
{
	uchar *pszRet;

	if(LocalHostNameOverride != NULL) {
		pszRet = LocalHostNameOverride;
		goto done;
	}

	if(LocalHostName == NULL)
		pszRet = (uchar*) "[localhost]";
	else {
		if(GetPreserveFQDN() == 1)
			pszRet = LocalFQDNName;
		else
			pszRet = LocalHostName;
	}
done:
	return(pszRet);
}


/* set our local domain name. Free previous domain, if it was already set.
 */
static rsRetVal
SetLocalDomain(uchar *newname)
{
	free(LocalDomain);
	LocalDomain = newname;
	return RS_RET_OK;
}


/* return our local hostname. if it is not set, "[localhost]" is returned
 */
static uchar*
GetLocalDomain(void)
{
	return LocalDomain;
}


/* generate the local hostname property. This must be done after the hostname info
 * has been set as well as PreserveFQDN.
 * rgerhards, 2009-06-30
 * NOTE: This function tries to avoid locking by not destructing the previous value
 * immediately. This is so that current readers can  continue to use the previous name.
 * Otherwise, we would need to use read/write locks to protect the update process.
 * In order to do so, we save the previous value and delete it when we are called again
 * the next time. Note that this in theory is racy and can lead to a double-free.
 * In practice, however, the window of exposure to trigger this is extremely short
 * and as this functions is very infrequently being called (on HUP), the trigger
 * condition for this bug is so highly unlikely that it never occurs in practice.
 * Probably if you HUP rsyslog every few milliseconds, but who does that...
 * To further reduce risk potential, we do only update the property when there
 * actually is a hostname change, which makes it even less likely.
 * rgerhards, 2013-10-28
 */
static rsRetVal
GenerateLocalHostNameProperty(void)
{
	uchar *pszPrev;
	int lenPrev;
	prop_t *hostnameNew;
	uchar *pszName;
	DEFiRet;

	if(propLocalHostNameToDelete != NULL)
		prop.Destruct(&propLocalHostNameToDelete);

	if(LocalHostNameOverride == NULL) {
		if(LocalHostName == NULL)
			pszName = (uchar*) "[localhost]";
		else {
			if(GetPreserveFQDN() == 1)
				pszName = LocalFQDNName;
			else
				pszName = LocalHostName;
		}
	} else { /* local hostname is overriden via config */
		pszName = LocalHostNameOverride;
	}
	DBGPRINTF("GenerateLocalHostName uses '%s'\n", pszName);

	if(propLocalHostName == NULL)
		pszPrev = (uchar*)""; /* make sure strcmp() below does not match */
	else
		prop.GetString(propLocalHostName, &pszPrev, &lenPrev);

	if(ustrcmp(pszPrev, pszName)) {
		/* we need to update */
		CHKiRet(prop.Construct(&hostnameNew));
		CHKiRet(prop.SetString(hostnameNew, pszName, ustrlen(pszName)));
		CHKiRet(prop.ConstructFinalize(hostnameNew));
		propLocalHostNameToDelete = propLocalHostName;
		propLocalHostName = hostnameNew;
	}

finalize_it:
	RETiRet;
}


/* return our local hostname as a string property
 */
static prop_t*
GetLocalHostNameProp(void)
{
	return(propLocalHostName);
}


static rsRetVal
SetLocalFQDNName(uchar *newname)
{
	free(LocalFQDNName);
	LocalFQDNName = newname;
	return RS_RET_OK;
}

/* return the current localhost name as FQDN (requires FQDN to be set) 
 * TODO: we should set the FQDN ourselfs in here!
 */
static uchar*
GetLocalFQDNName(void)
{
	return(LocalFQDNName == NULL ? (uchar*) "[localhost]" : LocalFQDNName);
}


/* return the current working directory */
static uchar*
GetWorkDir(void)
{
	return(pszWorkDir == NULL ? (uchar*) "" : pszWorkDir);
}

/* return the "raw" working directory, which means
 * NULL if unset.
 */
const uchar *
glblGetWorkDirRaw(void)
{
	return pszWorkDir;
}

/* return the current default netstream driver */
static uchar*
GetDfltNetstrmDrvr(void)
{
	return(pszDfltNetstrmDrvr == NULL ? DFLT_NETSTRM_DRVR : pszDfltNetstrmDrvr);
}


/* return the current default netstream driver CA File */
static uchar*
GetDfltNetstrmDrvrCAF(void)
{
	return(pszDfltNetstrmDrvrCAF);
}


/* return the current default netstream driver key File */
static uchar*
GetDfltNetstrmDrvrKeyFile(void)
{
	return(pszDfltNetstrmDrvrKeyFile);
}


/* return the current default netstream driver certificate File */
static uchar*
GetDfltNetstrmDrvrCertFile(void)
{
	return(pszDfltNetstrmDrvrCertFile);
}


/* [ar] Source IP for local client to be used on multihomed host */
static rsRetVal
SetSourceIPofLocalClient(uchar *newname)
{
	if(SourceIPofLocalClient != NULL) {
		free(SourceIPofLocalClient); }
	SourceIPofLocalClient = newname;
	return RS_RET_OK;
}

static uchar*
GetSourceIPofLocalClient(void)
{
	return(SourceIPofLocalClient);
}


/* queryInterface function
 * rgerhards, 2008-02-21
 */
BEGINobjQueryInterface(glbl)
CODESTARTobjQueryInterface(glbl)
	if(pIf->ifVersion != glblCURR_IF_VERSION) { /* check for current version, increment on each change */
		ABORT_FINALIZE(RS_RET_INTERFACE_NOT_SUPPORTED);
	}

	/* ok, we have the right interface, so let's fill it
	 * Please note that we may also do some backwards-compatibility
	 * work here (if we can support an older interface version - that,
	 * of course, also affects the "if" above).
	 */
	pIf->GetWorkDir = GetWorkDir;
	pIf->GenerateLocalHostNameProperty = GenerateLocalHostNameProperty;
	pIf->GetLocalHostNameProp = GetLocalHostNameProp;
	pIf->GetLocalHostIP = GetLocalHostIP;
	pIf->SetGlobalInputTermination = SetGlobalInputTermination;
	pIf->GetGlobalInputTermState = GetGlobalInputTermState;
	pIf->GetSourceIPofLocalClient = GetSourceIPofLocalClient;	/* [ar] */
	pIf->SetSourceIPofLocalClient = SetSourceIPofLocalClient;	/* [ar] */
#define SIMP_PROP(name) \
	pIf->Get##name = Get##name; \
	pIf->Set##name = Set##name;
	SIMP_PROP(MaxLine);
	SIMP_PROP(OptimizeUniProc);
	SIMP_PROP(ParseHOSTNAMEandTAG);
	SIMP_PROP(PreserveFQDN);
	SIMP_PROP(DefPFFamily);
	SIMP_PROP(DropMalPTRMsgs);
	SIMP_PROP(Option_DisallowWarning);
	SIMP_PROP(DisableDNS);
	SIMP_PROP(mainqCnfObj);
	SIMP_PROP(LocalFQDNName)
	SIMP_PROP(LocalHostName)
	SIMP_PROP(LocalDomain)
	SIMP_PROP(StripDomains)
	SIMP_PROP(LocalHosts)
	SIMP_PROP(ParserControlCharacterEscapePrefix)
	SIMP_PROP(ParserDropTrailingLFOnReception)
	SIMP_PROP(ParserEscapeControlCharactersOnReceive)
	SIMP_PROP(ParserSpaceLFOnReceive)
	SIMP_PROP(ParserEscape8BitCharactersOnReceive)
	SIMP_PROP(ParserEscapeControlCharacterTab)
	SIMP_PROP(ParserEscapeControlCharactersCStyle)
	SIMP_PROP(DfltNetstrmDrvr)
	SIMP_PROP(DfltNetstrmDrvrCAF)
	SIMP_PROP(DfltNetstrmDrvrKeyFile)
	SIMP_PROP(DfltNetstrmDrvrCertFile)
#ifdef USE_UNLIMITED_SELECT
	SIMP_PROP(FdSetSize)
#endif
#undef	SIMP_PROP
finalize_it:
ENDobjQueryInterface(glbl)


/* Reset config variables to default values.
 * rgerhards, 2008-04-17
 */
static rsRetVal resetConfigVariables(uchar __attribute__((unused)) *pp, void __attribute__((unused)) *pVal)
{
	free(pszDfltNetstrmDrvr);
	pszDfltNetstrmDrvr = NULL;
	free(pszDfltNetstrmDrvrCAF);
	pszDfltNetstrmDrvrCAF = NULL;
	free(pszDfltNetstrmDrvrKeyFile);
	pszDfltNetstrmDrvrKeyFile = NULL;
	free(pszDfltNetstrmDrvrCertFile);
	pszDfltNetstrmDrvrCertFile = NULL;
	free(LocalHostNameOverride);
	LocalHostNameOverride = NULL;
	free(pszWorkDir);
	pszWorkDir = NULL;
	bDropMalPTRMsgs = 0;
	bOptimizeUniProc = 1;
	bPreserveFQDN = 0;
	iMaxLine = 8192;
	cCCEscapeChar = '#';
	bDropTrailingLF = 1;
	bEscapeCCOnRcv = 1; /* default is to escape control characters */
	bSpaceLFOnRcv = 0;
	bEscape8BitChars = 0; /* default is not to escape control characters */
	bEscapeTab = 1; /* default is to escape tab characters */
	bParserEscapeCCCStyle = 0;
#ifdef USE_UNLIMITED_SELECT
	iFdSetSize = howmany(FD_SETSIZE, __NFDBITS) * sizeof (fd_mask);
#endif
	return RS_RET_OK;
}


/* Prepare for new config
 */
void
glblPrepCnf(void)
{
	free(mainqCnfObj);
	mainqCnfObj = NULL;
	free(cnfparamvals);
	cnfparamvals = NULL;
}

/* handle a global config object. Note that multiple global config statements
 * are permitted (because of plugin support), so once we got a param block,
 * we need to hold to it.
 * rgerhards, 2011-07-19
 */
void
glblProcessCnf(struct cnfobj *o)
{
	int i;

	cnfparamvals = nvlstGetParams(o->nvlst, &paramblk, cnfparamvals);
	dbgprintf("glbl param blk after glblProcessCnf:\n");
	cnfparamsPrint(&paramblk, cnfparamvals);

	/* The next thing is a bit hackish and should be changed in higher
	 * versions. There are a select few parameters which we need to
	 * act on immediately. These are processed here.
	 */
	for(i = 0 ; i < paramblk.nParams ; ++i) {
		if(!cnfparamvals[i].bUsed)
			continue;
		if(!strcmp(paramblk.descr[i].name, "processinternalmessages")) {
			bProcessInternalMessages = (int) cnfparamvals[i].val.d.n;
		}
	}
}

/* Set mainq parameters. Note that when this is not called, we'll use the
 * legacy parameter config. mainq parameters can only be set once.
 */
void
glblProcessMainQCnf(struct cnfobj *o)
{
	if(mainqCnfObj == NULL) {
		mainqCnfObj = o;
	} else {
		errmsg.LogError(0, RS_RET_ERR, "main_queue() object can only be specified "
				"once - all but first ignored\n");
	}
}

/* destruct the main q cnf object after it is no longer needed. This is
 * also used to do some final checks.
 */
void
glblDestructMainqCnfObj()
{
	/* Only destruct if not NULL! */
	if (mainqCnfObj != NULL) {
		nvlstChkUnused(mainqCnfObj->nvlst);
	}
	cnfobjDestruct(mainqCnfObj);
	mainqCnfObj = NULL;
}


/* This processes the "regular" parameters which are to be set after the
 * config has been fully loaded.
 */
void
glblDoneLoadCnf(void)
{
	int i;
	unsigned char *cstr;

	if(cnfparamvals == NULL)
		goto finalize_it;

	for(i = 0 ; i < paramblk.nParams ; ++i) {
		if(!cnfparamvals[i].bUsed)
			continue;
		if(!strcmp(paramblk.descr[i].name, "workdirectory")) {
			cstr = (uchar*) es_str2cstr(cnfparamvals[i].val.d.estr, NULL);
			setWorkDir(NULL, cstr);
		} else if(!strcmp(paramblk.descr[i].name, "localhostname")) {
			free(LocalHostNameOverride);
			LocalHostNameOverride = (uchar*)
				es_str2cstr(cnfparamvals[i].val.d.estr, NULL);
		} else if(!strcmp(paramblk.descr[i].name, "defaultnetstreamdriverkeyfile")) {
			free(pszDfltNetstrmDrvrKeyFile);
			pszDfltNetstrmDrvrKeyFile = (uchar*)
				es_str2cstr(cnfparamvals[i].val.d.estr, NULL);
		} else if(!strcmp(paramblk.descr[i].name, "defaultnetstreamdrivercafile")) {
			free(pszDfltNetstrmDrvrCAF);
			pszDfltNetstrmDrvrCAF = (uchar*)
				es_str2cstr(cnfparamvals[i].val.d.estr, NULL);
		} else if(!strcmp(paramblk.descr[i].name, "defaultnetstreamdriver")) {
			free(pszDfltNetstrmDrvr);
			pszDfltNetstrmDrvr = (uchar*)
				es_str2cstr(cnfparamvals[i].val.d.estr, NULL);
		} else if(!strcmp(paramblk.descr[i].name, "preservefqdn")) {
			bPreserveFQDN = (int) cnfparamvals[i].val.d.n;
		} else if(!strcmp(paramblk.descr[i].name,
				"dropmsgswithmaliciousdnsptrrecords")) {
			bDropMalPTRMsgs = (int) cnfparamvals[i].val.d.n;
		} else if(!strcmp(paramblk.descr[i].name, "action.reportsuspension")) {
			bActionReportSuspension = (int) cnfparamvals[i].val.d.n;
		} else if(!strcmp(paramblk.descr[i].name, "maxmessagesize")) {
			iMaxLine = (int) cnfparamvals[i].val.d.n;
		} else if(!strcmp(paramblk.descr[i].name, "debug.onshutdown")) {
			glblDebugOnShutdown = (int) cnfparamvals[i].val.d.n;
			errmsg.LogError(0, RS_RET_OK, "debug: onShutdown set to %d", glblDebugOnShutdown);
		} else if(!strcmp(paramblk.descr[i].name, "parser.controlcharacterescapeprefix")) {
			cCCEscapeChar = (uchar) *es_str2cstr(cnfparamvals[i].val.d.estr, NULL);
		} else if(!strcmp(paramblk.descr[i].name, "parser.droptrailinglfonreception")) {
			bDropTrailingLF = (int) cnfparamvals[i].val.d.n;
		} else if(!strcmp(paramblk.descr[i].name, "parser.escapecontrolcharactersonreceive")) {
			bEscapeCCOnRcv = (int) cnfparamvals[i].val.d.n;
		} else if(!strcmp(paramblk.descr[i].name, "parser.spacelfonreceive")) {
			bSpaceLFOnRcv = (int) cnfparamvals[i].val.d.n;
		} else if(!strcmp(paramblk.descr[i].name, "parser.escape8bitcharactersonreceive")) {
			bEscape8BitChars = (int) cnfparamvals[i].val.d.n;
		} else if(!strcmp(paramblk.descr[i].name, "parser.escapecontrolcharactertab")) {
			bEscapeTab = (int) cnfparamvals[i].val.d.n;
		} else if(!strcmp(paramblk.descr[i].name, "parser.escapecontrolcharacterscstyle")) {
			bParserEscapeCCCStyle = (int) cnfparamvals[i].val.d.n;
		} else if(!strcmp(paramblk.descr[i].name, "debug.logfile")) {
			if(pszAltDbgFileName == NULL) {
				pszAltDbgFileName = es_str2cstr(cnfparamvals[i].val.d.estr, NULL);
				if((altdbg = open(pszAltDbgFileName, O_WRONLY|O_CREAT|O_TRUNC|O_NOCTTY|O_CLOEXEC, S_IRUSR|S_IWUSR)) == -1) {
					errmsg.LogError(0, RS_RET_ERR, "debug log file '%s' could not be opened", pszAltDbgFileName);
				}
			}
			errmsg.LogError(0, RS_RET_OK, "debug log file is '%s', fd %d", pszAltDbgFileName, altdbg);
		} else {
			dbgprintf("glblDoneLoadCnf: program error, non-handled "
			  "param '%s'\n", paramblk.descr[i].name);
		}
	}

	if(glblDebugOnShutdown && Debug != DEBUG_FULL) {
		Debug = DEBUG_ONDEMAND;
		stddbg = -1;
	}
finalize_it:	return;
}


/* Initialize the glbl class. Must be called as the very first method
 * before anything else is called inside this class.
 * rgerhards, 2008-02-19
 */
BEGINAbstractObjClassInit(glbl, 1, OBJ_IS_CORE_MODULE) /* class, version */
	/* request objects we use */
	CHKiRet(objUse(prop, CORE_COMPONENT));
	CHKiRet(objUse(errmsg, CORE_COMPONENT));

	/* config handlers are never unregistered and need not be - we are always loaded ;) */
	CHKiRet(regCfSysLineHdlr((uchar *)"debugfile", 0, eCmdHdlrGetWord, setDebugFile, NULL, NULL));
	CHKiRet(regCfSysLineHdlr((uchar *)"debuglevel", 0, eCmdHdlrInt, setDebugLevel, NULL, NULL));
	CHKiRet(regCfSysLineHdlr((uchar *)"workdirectory", 0, eCmdHdlrGetWord, setWorkDir, NULL, NULL));
	CHKiRet(regCfSysLineHdlr((uchar *)"dropmsgswithmaliciousdnsptrrecords", 0, eCmdHdlrBinary, NULL, &bDropMalPTRMsgs, NULL));
	CHKiRet(regCfSysLineHdlr((uchar *)"defaultnetstreamdriver", 0, eCmdHdlrGetWord, NULL, &pszDfltNetstrmDrvr, NULL));
	CHKiRet(regCfSysLineHdlr((uchar *)"defaultnetstreamdrivercafile", 0, eCmdHdlrGetWord, NULL, &pszDfltNetstrmDrvrCAF, NULL));
	CHKiRet(regCfSysLineHdlr((uchar *)"defaultnetstreamdriverkeyfile", 0, eCmdHdlrGetWord, NULL, &pszDfltNetstrmDrvrKeyFile, NULL));
	CHKiRet(regCfSysLineHdlr((uchar *)"defaultnetstreamdrivercertfile", 0, eCmdHdlrGetWord, NULL, &pszDfltNetstrmDrvrCertFile, NULL));
	CHKiRet(regCfSysLineHdlr((uchar *)"localhostname", 0, eCmdHdlrGetWord, NULL, &LocalHostNameOverride, NULL));
	CHKiRet(regCfSysLineHdlr((uchar *)"localhostipif", 0, eCmdHdlrGetWord, setLocalHostIPIF, NULL, NULL));
	CHKiRet(regCfSysLineHdlr((uchar *)"optimizeforuniprocessor", 0, eCmdHdlrBinary, NULL, &bOptimizeUniProc, NULL));
	CHKiRet(regCfSysLineHdlr((uchar *)"preservefqdn", 0, eCmdHdlrBinary, NULL, &bPreserveFQDN, NULL));
	CHKiRet(regCfSysLineHdlr((uchar *)"maxmessagesize", 0, eCmdHdlrSize, NULL, &iMaxLine, NULL));

	/* Deprecated parser config options */
	CHKiRet(regCfSysLineHdlr((uchar *)"controlcharacterescapeprefix", 0, eCmdHdlrGetChar, NULL, &cCCEscapeChar, NULL));
	CHKiRet(regCfSysLineHdlr((uchar *)"droptrailinglfonreception", 0, eCmdHdlrBinary, NULL, &bDropTrailingLF, NULL));
	CHKiRet(regCfSysLineHdlr((uchar *)"escapecontrolcharactersonreceive", 0, eCmdHdlrBinary, NULL, &bEscapeCCOnRcv, NULL));
	CHKiRet(regCfSysLineHdlr((uchar *)"spacelfonreceive", 0, eCmdHdlrBinary, NULL, &bSpaceLFOnRcv, NULL));
	CHKiRet(regCfSysLineHdlr((uchar *)"escape8bitcharactersonreceive", 0, eCmdHdlrBinary, NULL, &bEscape8BitChars, NULL));
	CHKiRet(regCfSysLineHdlr((uchar *)"escapecontrolcharactertab", 0, eCmdHdlrBinary, NULL, &bEscapeTab, NULL));

	CHKiRet(regCfSysLineHdlr((uchar *)"resetconfigvariables", 1, eCmdHdlrCustomHandler, resetConfigVariables, NULL, NULL));

	INIT_ATOMIC_HELPER_MUT(mutTerminateInputs);
ENDObjClassInit(glbl)


/* Exit the glbl class.
 * rgerhards, 2008-04-17
 */
BEGINObjClassExit(glbl, OBJ_IS_CORE_MODULE) /* class, version */
	free(pszDfltNetstrmDrvr);
	free(pszDfltNetstrmDrvrCAF);
	free(pszDfltNetstrmDrvrKeyFile);
	free(pszDfltNetstrmDrvrCertFile);
	free(pszWorkDir);
	free(LocalDomain);
	free(LocalHostName);
	free(LocalHostNameOverride);
	free(LocalFQDNName);
	objRelease(prop, CORE_COMPONENT);
	if(propLocalHostNameToDelete != NULL)
		prop.Destruct(&propLocalHostNameToDelete);
	DESTROY_ATOMIC_HELPER_MUT(mutTerminateInputs);
ENDObjClassExit(glbl)

void glblProcessCnf(struct cnfobj *o);

/* vi:set ai:
 */
