/*******************************************************************************
 * Copyright (c) 2015-2016 Florian Froschermeier <florian.froschermeier@tum.de>
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * which accompanies this distribution, and is available at
 * http://www.eclipse.org/legal/epl-v10.html
 *
 * Contributors:
 *    Florian Froschermeier
 *      - initial integration of the OPC-UA protocol
 *******************************************************************************/


#include "opcuahandler.h"
#include <commfb.h>
#include <devexec.h>
#include "../../core/utils/criticalregion.h"

/*#ifdef FORTE_COM_OPC_UA_ENABLE_INIT_NAMESPACE
#include <open62541/build/src_generated/ua_namespaceinit_generated.h>
#endif*/

using namespace forte::com_infra;

DEFINE_SINGLETON(COPC_UA_Handler);

#define FORTE_COM_OPC_UA_PORT 4840

const int COPC_UA_Handler::scmUADataTypeMapping[] = {
		/* Datatype mapping of IEC61131 types to OPC-UA types according
		 * to OPC UA standard specification release 1.0,
		 * PLCOpen-OPC-UA-"Information Model" Table 26, Section 5.2 Datatypes;
		 */

		UA_TYPES_VARIANT, //e_ANY,
		UA_TYPES_BOOLEAN, //e_BOOL,
		UA_TYPES_SBYTE, //e_SINT,
		UA_TYPES_INT16,	//e_INT
		UA_TYPES_INT32, //e_DINT
		UA_TYPES_INT64, //e_LINT
		UA_TYPES_BYTE, //e_USINT,
		UA_TYPES_UINT16, //e_UINT
		UA_TYPES_UINT32, //e_UDINT
		UA_TYPES_UINT64, //e_ULINT
		UA_TYPES_BYTE, //e_BYTE
		UA_TYPES_UINT16, //e_WORD
		UA_TYPES_UINT32, //e_DWORD
		UA_TYPES_UINT64, //e_LWORD
		UA_TYPES_DATETIME, //e_DATE,
		UA_TYPES_DATETIME, //e_TIME_OF_DAY,
		UA_TYPES_DATETIME, //e_DATE_AND_TIME,
		UA_TYPES_DOUBLE, //e_TIME, //until here simple Datatypes
		UA_TYPES_FLOAT, //e_REAL
		UA_TYPES_DOUBLE, //e_LREAL
		UA_TYPES_STRING, //e_STRING
		UA_TYPES_STRING //e_WSTRING,

		//FIXME add mapping for following datatypes.
		//e_DerivedData,
		//e_DirectlyDerivedData,
		//e_EnumeratedData,
		//e_SubrangeData,
		//e_ARRAY, //according to the compliance profile
		//e_STRUCT,
		//e_External = 256, // Base for CIEC_ANY based types outside of the forte base
		//e_Max = 65535 // Guarantees at least 16 bits - otherwise gcc will optimizes on some platforms
};

void COPC_UA_Handler::configureUAServer(TForteUInt16 UAServerPort) {
	m_server_config = UA_ServerConfig_standard;
	m_server_config.enableUsernamePasswordLogin = false;
	m_server_config.networkLayersSize = 1;
	m_server_config.logger = UA_Log_Stdout;

	m_server_networklayer = UA_ServerNetworkLayerTCP(UA_ConnectionConfig_standard, UAServerPort);
	m_server_config.networkLayers = &m_server_networklayer;
}

COPC_UA_Handler::COPC_UA_Handler() : m_server_config(), m_server_networklayer(), getNodeForPathMutex(){
	configureUAServer(FORTE_COM_OPC_UA_PORT); 	// configure a standard server
	mOPCUAServer = UA_Server_new(m_server_config);

	setServerRunning();		// set server loop flag

	if(!isAlive()){
		//thread is not running start it
		start();
	}

	// OPTION: add a namespace in xml format to the server containing the application configuration.
	//UA_Server_addExternalNamespace()
}

COPC_UA_Handler::~COPC_UA_Handler() {
	stopServerRunning();
	UA_Server_delete(mOPCUAServer);
	m_server_networklayer.deleteMembers(&m_server_networklayer);
}

void COPC_UA_Handler::run(){
	UA_StatusCode retVal = UA_Server_run(mOPCUAServer, mbServerRunning);	// server keeps iterating as long as running is true;
	DEVLOG_INFO("UA_Server run status code %s", retVal);
}

void COPC_UA_Handler::enableHandler(void){
	start();
}

void COPC_UA_Handler::disableHandler(void){
	COPC_UA_Handler::stopServerRunning();
	end();
}

void COPC_UA_Handler::setPriority(int){
	//currently we are doing nothing here.
	//TODO We should adjust the thread priority.
}

int COPC_UA_Handler::getPriority(void) const{
	//the same as for setPriority
	return 0;
}

UA_Server * COPC_UA_Handler::getServer(){
	return mOPCUAServer;
}


void COPC_UA_Handler::setServerRunning(){
	*mbServerRunning = UA_TRUE;
}

void COPC_UA_Handler::stopServerRunning(){
	*mbServerRunning = UA_FALSE;
}


void COPC_UA_Handler::registerNode(){
}


/*
 * Get Function Block Node Id from the pointer to a CFunctionBlock.
 * Method is used to check if a not to the pointed function block already
 * exists in the address space of the OPC-UA Server.
 */

UA_StatusCode COPC_UA_Handler::getFBNodeId(const CFunctionBlock* pCFB, UA_NodeId* returnFBNodeId){
	const char* FBInstanceName = pCFB->getInstanceName();	// Name of the SourcePoint function block
	UA_NodeId FBNodeId = UA_NODEID_STRING_ALLOC(1, FBInstanceName);		// Create new FBNodeId from c string

	UA_NodeId* returnNodeId = UA_NodeId_new();
	UA_StatusCode retVal = UA_Server_readNodeId(mOPCUAServer, FBNodeId, returnNodeId);		// read node of given ID
	if(retVal != UA_STATUSCODE_GOOD){
		return retVal;		// reading not successful
	}else{
		retVal = UA_NodeId_copy(returnNodeId, returnFBNodeId);	// reading successful, return NodeId
	};
	return retVal;
}


UA_StatusCode COPC_UA_Handler::getSPNodeId(const CFunctionBlock *pCFB, SConnectionPoint& sourceRD, UA_NodeId* returnSPNodeId){

	// Reading the node without reference to parent node id, unknown if this works.
	//FIXME needs further testing with OPC_UA Address Space Browser and example node
	const SFBInterfaceSpec* sourceFBInterface = pCFB->getFBInterfaceSpec();

	CStringDictionary::TStringId SPNameId = sourceFBInterface->m_aunDONames[sourceRD.mPortId];
	const char * SPName = CStringDictionary::getInstance().get(SPNameId);

	UA_NodeId SPNodeId = UA_NODEID_STRING_ALLOC(1, SPName);

	UA_NodeId* returnNodeId = UA_NodeId_new();
	UA_StatusCode retVal = UA_Server_readNodeId(mOPCUAServer,SPNodeId, returnNodeId);		// read node of given ID
	if(retVal != UA_STATUSCODE_GOOD){
		return retVal;		// reading not successful
	}else{
		retVal = UA_NodeId_copy(returnNodeId, returnSPNodeId);	// reading successful, return NodeId
	};
	return retVal;
}


UA_NodeId* COPC_UA_Handler::getNodeForPath(char* nodePath, bool createIfNotFound) {


	// remove tailing slash
	size_t pathLen = strlen(nodePath);
	while (pathLen && nodePath[pathLen-1] == '/') {
		nodePath[pathLen - 1] = 0;
		pathLen--;
	}
	if (pathLen == 0)
		return nullptr;

	// count number of folders in node path
	unsigned int folderCnt = 0;
	char *c = nodePath;
	while (*c) {
		if (*c == '/')
			folderCnt++;
		c++;
	}

	UA_NodeId parent;

	char *fullPath = strdup(nodePath);
	char *tok = strtok (nodePath,"/");
	if (strcmp(tok, "Objects") != 0 && strcmp(tok, "0:Objects") != 0) {
		DEVLOG_ERROR("Node path '%s' has to start with '/Objects'", fullPath);
		free(fullPath);
		return nullptr;
	}

	parent = UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER);
	folderCnt--; //remaining count without Objects folder

	// create a client for requesting the nodes
	UA_Client *client = UA_Client_new(UA_ClientConfig_standard);

	char localEndpoint[28];
	snprintf(localEndpoint,28, "opc.tcp://localhost:%d", FORTE_COM_OPC_UA_PORT);

	if(UA_Client_connect(client, localEndpoint) != UA_STATUSCODE_GOOD) {
		DEVLOG_ERROR("Could not connect to local OPC UA Server");
		UA_Client_delete(client);
		free(fullPath);
		return nullptr;
	}


	// for every folder (which is a BrowsePath) we want to get the node id
	UA_BrowsePath *browsePaths = static_cast<UA_BrowsePath*>(malloc(sizeof(UA_BrowsePath) * folderCnt));

	for (unsigned int i=0; i<folderCnt; i++) {
		tok = strtok(NULL,"/");
		UA_BrowsePath_init(&browsePaths[i]);
		browsePaths[i].startingNode = parent;
		browsePaths[i].relativePath.elementsSize = i+1;
		browsePaths[i].relativePath.elements = static_cast<UA_RelativePathElement*>(malloc(sizeof(UA_RelativePathElement)*(i+1)));
		for (unsigned int j=0; j<=i; j++) {

			if (j<i) {
				// just copy from before
				UA_RelativePathElement_copy(&browsePaths[i-1].relativePath.elements[j], &browsePaths[i].relativePath.elements[j]);
				continue;
			}

			// the last element is a new one

			UA_RelativePathElement_init(&browsePaths[i].relativePath.elements[j]);
			browsePaths[i].relativePath.elements[j].isInverse = UA_TRUE;

			// split the qualified name
			char *splitPos = tok;
			while (*splitPos && *splitPos != ':') {
				splitPos++;
			}

			// default namespace is 0
			UA_UInt16 ns = 0;
			char *targetName = tok;

			if (*splitPos) {
				// namespace given
				ns = static_cast<UA_UInt16>(atoi(tok));
				targetName = ++splitPos;
			}
			browsePaths[i].relativePath.elements[j].targetName = UA_QUALIFIEDNAME(ns, strdup(targetName));
		}
	}

	UA_TranslateBrowsePathsToNodeIdsRequest request;
	UA_TranslateBrowsePathsToNodeIdsRequest_init(&request);
	request.browsePaths = browsePaths;
	request.browsePathsSize = folderCnt;

	{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
		// other thready may currently create nodes for the same path, thus mutex
		CCriticalRegion criticalRegion(this->getNodeForPathMutex);
#pragma GCC diagnostic pop
		UA_TranslateBrowsePathsToNodeIdsResponse response = UA_Client_Service_translateBrowsePathsToNodeIds(client, request);

		if (response.responseHeader.serviceResult != UA_STATUSCODE_GOOD) {
			DEVLOG_ERROR("Could not translate browse paths for '%s' to node IDs. Service returned: 0x%08x", fullPath, response.responseHeader.serviceResult);
			UA_TranslateBrowsePathsToNodeIdsRequest_deleteMembers(&request);
			UA_TranslateBrowsePathsToNodeIdsResponse_deleteMembers(&response);
			UA_Client_disconnect(client);
			UA_Client_delete(client);
			free(fullPath);
			return nullptr;
		}

		if (response.resultsSize != folderCnt) {
			DEVLOG_ERROR("Could not translate browse paths for '%s' to node IDs. resultSize (%d) != expected count (%d)", fullPath, response.resultsSize,
						 folderCnt);
			UA_TranslateBrowsePathsToNodeIdsRequest_deleteMembers(&request);
			UA_TranslateBrowsePathsToNodeIdsResponse_deleteMembers(&response);
			UA_Client_disconnect(client);
			UA_Client_delete(client);
			free(fullPath);
			return nullptr;
		}

		UA_NodeId *foundNodeId = nullptr;

		if (response.results[folderCnt-1].statusCode == UA_STATUSCODE_GOOD) {
			foundNodeId = static_cast<UA_NodeId*>(malloc(sizeof(UA_NodeId)));
			UA_NodeId_copy(&response.results[folderCnt-1].targets[0].targetId.nodeId, foundNodeId);
		} else if (createIfNotFound) {
			// last node does not exist, and we should create the path
			// skip last node because we already know that it doesn't exist

			foundNodeId = static_cast<UA_NodeId*>(malloc(sizeof(UA_NodeId)));
			UA_NodeId_init(foundNodeId);
			int i;
			for (i = folderCnt - 2; i >= 0; i--) {
				if (response.results[i].statusCode != UA_STATUSCODE_GOOD) {
					// find first existing node
					continue;
				}
				// now we found the first existing node
				if (response.results[i].targetsSize == 0) {
					DEVLOG_ERROR("Could not translate browse paths for '%s' to node IDs. target size is 0.", fullPath);
					break;
				}
				if (response.results[i].targetsSize > 1) {
					DEVLOG_WARNING("The given browse path '%s' has multiple results for the same path. Taking the first result.", fullPath);
				}

				// foundNodeId contains the ID of the parent which exists
				UA_NodeId_copy(&response.results[i].targets[0].targetId.nodeId, foundNodeId);
				break;
			}

			if (i==-1) {
				// no node of the path exists, thus parent is Objects folder
				UA_NodeId_copy(&parent, foundNodeId);
			}
			i++;

			// create all the nodes on the way
			for (unsigned int j=(unsigned int)i; j<folderCnt; j++) {

				// the last browse path contains all relativePath elements.
				UA_QualifiedName *targetName = &request.browsePaths[folderCnt-1].relativePath.elements[j].targetName;

				UA_ObjectAttributes oAttr;
				UA_ObjectAttributes_init(&oAttr);
				char locale[] = "en_US";
				char *nodeName = static_cast<char*>(malloc(sizeof(char)*targetName->name.length+1));
				memcpy(nodeName, targetName->name.data, targetName->name.length);
				nodeName[targetName->name.length] = 0;
				oAttr.description = UA_LOCALIZEDTEXT(locale, nodeName);
				oAttr.displayName = UA_LOCALIZEDTEXT(locale, nodeName);
				UA_StatusCode retval;
				if ((retval = UA_Server_addObjectNode(mOPCUAServer, UA_NODEID_NUMERIC(1, 0),
										*foundNodeId, UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
										*targetName, UA_NODEID_NUMERIC(0, UA_NS0ID_FOLDERTYPE), oAttr, NULL, foundNodeId)) != UA_STATUSCODE_GOOD) {
					DEVLOG_ERROR("Could not addObjectNode. Status: 0x%08x", retval );
					free(foundNodeId);
					foundNodeId = nullptr;
					break;
				}
			}
		}

		UA_TranslateBrowsePathsToNodeIdsRequest_deleteMembers(&request);
		UA_TranslateBrowsePathsToNodeIdsResponse_deleteMembers(&response);
		UA_Client_disconnect(client);
		UA_Client_delete(client);
		free(fullPath);
		return foundNodeId;
	}
}




///* Method assembleUANodeId is used to parse the Reference NodeId of Publisher and Subscriber FunctionBlocks.
// * The ParamId is of the following format: opc_ua[address:port];NamespaceIndex:IdentifierType:Identifier
// * Example: opc_ua[127.0.0.1:16664;2:String:Q;2:String:G]
// */
////pass the charecter string after the
//UA_StatusCode COPC_UA_Handler::assembleUANodeId(char* NodeIdString, UA_NodeId *returnNodeId){
//	UA_StatusCode retVal = UA_STATUSCODE_GOOD;
//
//	UA_NodeId * ReferenceId = UA_NodeId_new();
//	UA_NodeId_init(ReferenceId);
//
//
//	/*   UA_NodeIdTypes
//	 *    UA_NODEIDTYPE_NUMERIC    = 0,  In the binary encoding, this can also become 1 or 2
//	 *                                     (2byte and 4byte encoding of small numeric nodeids)
//	 *    UA_NODEIDTYPE_STRING     = 3,
//	 *    UA_NODEIDTYPE_GUID       = 4,
//	 *    UA_NODEIDTYPE_BYTESTRING = 5
//	 */
//	// Example ParamIds: (2:string:Q)(2:numeric:Q)(2:guid:Q)(2:bytestring:Q)
//	char *pch;
//	int i = 0;
//	pch = strtok(NodeIdString,":");
//	while (pch != NULL) {
//		i++;
//		if(i == 1){
//			/* Assign NodeId namespace index */
//			ReferenceId->namespaceIndex = (UA_UInt16)atoi(pch);
//
//		} else if (i == 2){
//			/* Assign NodeId identifier types */
//			if ( !strcmp("numeric",pch)){
//				ReferenceId->identifierType = UA_NODEIDTYPE_NUMERIC;
//			}
//			else if ( !strcmp("string",pch)){
//				ReferenceId->identifierType = UA_NODEIDTYPE_STRING;
//			}
//			else if (!strcmp("guid",pch)){
//				ReferenceId->identifierType = UA_NODEIDTYPE_GUID;
//			}
//			else if (!strcmp("bytestring",pch)){
//				ReferenceId->identifierType = UA_NODEIDTYPE_BYTESTRING;
//			}
//			else {
//				return 0;
//			}
//
//		} else if (i == 3){
//			/* Assign NodeId identifier */
//			switch (ReferenceId->identifierType) {
//			case 0:
//				ReferenceId->identifier.numeric = atoi(pch);
//				break;
//			case 3:
//				ReferenceId->identifier.string = UA_STRING(pch);
//				break;
//				// TODO: missing mapping to type struct
//				/*case 4:
//				ReferenceId->identifier.guid = (UA_Guid) pch;
//				break;
//			case 5:
//				ReferenceId->identifier.byteString = (UA_ByteString) pch;
//				break;
//				 */
//			default:
//				break;
//			}
//
//		};
//		pch = strtok (NULL, ":");
//
//	};
//	retVal = UA_NodeId_copy(ReferenceId, returnNodeId);	// NodeId successfully created
//	return retVal;
//}



/* Function creates an address space object node defined by a given pointer to
 * a control function block. If creation successful the NodeId is returned otherwise
 * UA_StatusCode from node creation with error message.
 */
UA_StatusCode COPC_UA_Handler::createUAObjNode(const CFunctionBlock* pCFB, UA_NodeId * returnObjNodeId){

	// retrieve parent function block name
	CStringDictionary::TStringId sourceFBNameId = pCFB->getInstanceNameId();
	const char* srcFBName = CStringDictionary::getInstance().get(sourceFBNameId);

	// set UA NodeId attributes
	UA_NodeId newObjNodeId = UA_NODEID_STRING_ALLOC(1, srcFBName);
	UA_NodeId parentNodeId = UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER);
	UA_NodeId referenceTypeId = UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES);
	UA_QualifiedName objBrowseName = UA_QUALIFIEDNAME_ALLOC(0, srcFBName);
	UA_NodeId objTypeDefinition = UA_NODEID_NUMERIC(0, UA_NS0ID_FOLDERTYPE);

	UA_ObjectAttributes obj_attr;
	UA_ObjectAttributes_init(&obj_attr);
	char dispName[32];
	sprintf(dispName, "FB1-%s\n", srcFBName);
	char locale[] = "en_US";
	obj_attr.displayName = UA_LOCALIZEDTEXT(locale, dispName);
	char descpName[64];
	sprintf(descpName, "Object node of FB1-%s, origin: Publisher\n", srcFBName);

	obj_attr.description =  UA_LOCALIZEDTEXT(locale, descpName);

	UA_InstantiationCallback* instCallback = NULL; //((void *)0);	// Nullpointer as callback for this Node
	UA_NodeId * returnNodeId = UA_NodeId_new();

	// Add Object Node to the Server.
	UA_StatusCode retVal = UA_Server_addObjectNode(
			mOPCUAServer,                 // server
			newObjNodeId,              	  // requestedNewNodeId
			parentNodeId,                 // parentNodeId
			referenceTypeId,        	  // referenceTypeId
			objBrowseName,                // browseName
			objTypeDefinition,            // typeDefinition
			obj_attr,                     // Variable attributes
			instCallback,                 // instantiation callback
			returnNodeId);			  	  // return Node Id

	if(retVal == UA_STATUSCODE_GOOD){
		DEVLOG_INFO("UA-Server AddressSpace: New Object Node - %s added.\n", dispName);
		retVal = UA_NodeId_copy(returnNodeId, returnObjNodeId);
	}else{
		DEVLOG_INFO("UA-Server AddressSpace: Adding Object Node %s failed. Message: %x\n", dispName, retVal);
	}
	return retVal;
}

/* For a given connection SourcePoint between two 61499 FBs add
 * a variable Node to the OPC_UA address space.
 * Node is described by the name of the port and the name of the parent function block.
 * If creation successful the NodeId is returned otherwise
 * UA_StatusCode from node creation with error message.
 */
UA_StatusCode COPC_UA_Handler::createUAVarNode(const CFunctionBlock* pCFB, SConnectionPoint& sourceRD, UA_NodeId * returnVarNodeId){
	// retrieve parent function block name
	CStringDictionary::TStringId sourceFBNameId = pCFB->getInstanceNameId();
	const char* srcFBName = CStringDictionary::getInstance().get(sourceFBNameId);

	// get parent function block interface
	const SFBInterfaceSpec* sourceFBInterface = pCFB->getFBInterfaceSpec();

	// retrieve source port name
	CStringDictionary::TStringId SPNameId = sourceFBInterface->m_aunDONames[sourceRD.mPortId];
	const char * SPName = CStringDictionary::getInstance().get(SPNameId);

	// set UA NodeId attributes
	UA_NodeId newVarNodeId = UA_NODEID_STRING_ALLOC(1,SPName);
	UA_NodeId parentNodeId = UA_NODEID_STRING_ALLOC(1, srcFBName);
	UA_NodeId referenceTypeId = UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT);
	char browsename[32];
	sprintf(browsename, "Test-%s\n", SPName);
	UA_QualifiedName varBrowseName = UA_QUALIFIEDNAME(1, browsename);
	UA_NodeId typeDefinition = UA_NODEID_NULL;

	// attribute value
	UA_UInt32 myInteger = 42;

	// create variable attributes
	UA_VariableAttributes var_attr;
	UA_VariableAttributes_init(&var_attr);

	char display[32];
	sprintf(display, "SD-%s\n", SPName);

	char locale[] = "en_US";
	char description[] = "SD port of Publisher";

	var_attr.displayName = UA_LOCALIZEDTEXT(locale, display);
	var_attr.description = UA_LOCALIZEDTEXT(locale, description);
	UA_Variant_setScalar(&var_attr.value, &myInteger, &UA_TYPES[UA_TYPES_INT32]);

	UA_InstantiationCallback* instCallback = NULL; //((void *)0);	// Nullpointer as callback for this Node
	UA_NodeId * returnNodeId = UA_NodeId_new();

	// add UA Variable Node to the server address space
	UA_StatusCode retVal = UA_Server_addVariableNode(
			mOPCUAServer,                 // server
			newVarNodeId,              	  // requestedNewNodeId
			parentNodeId,                 // parentNodeId
			referenceTypeId,        	  // referenceTypeId   Reference to the type definition for the variable node
			varBrowseName,                // browseName
			typeDefinition,               // typeDefinition
			var_attr,                     // Variable attributes
			instCallback,                 // instantiation callback
			returnNodeId);			  	  // return Node Id


	if(retVal == UA_STATUSCODE_GOOD){
		DEVLOG_INFO("UA-Server AddressSpace: New Variable Node - %s added.\n", browsename);
		retVal = UA_NodeId_copy(returnNodeId, returnVarNodeId);
	}else{
		DEVLOG_INFO("UA-Server AddressSpace: Adding Variable Node %s failed. Message: %x\n", browsename, retVal);
	}
	return retVal;
}

/*
 * Update UA Address Space node value given by the data pointer to an IEC61499 data object.
 * Mapping of IEC61499 to OPC-UA types is performed by scmUADataTypeMapping array.
 */
UA_StatusCode COPC_UA_Handler::updateNodeValue(UA_NodeId * pNodeId, CIEC_ANY &paDataPoint){
	UA_Variant* NodeValue = UA_Variant_new();
	UA_Variant_init(NodeValue);

	// TODO make sure index of dataType in scmUADataTypeMapping is in range
	UA_Variant_setScalarCopy(NodeValue, static_cast<const void *>(paDataPoint.getConstDataPtr()),
			&UA_TYPES[scmUADataTypeMapping[paDataPoint.getDataTypeID()]]);
	return UA_Server_writeValue(mOPCUAServer, *(pNodeId), *(NodeValue));
}


/* Register a callback routine to a Node in the Address Space that is executed
 * on either write or read access on the node. A handle to the caller communication layer
 * is passed too. This alleviates for the process of searching the
 * originating layer of the external event.
 */
UA_StatusCode COPC_UA_Handler::registerNodeCallBack(UA_NodeId *paNodeId, forte::com_infra::CComLayer *paLayer){
	UA_ValueCallback callback = {static_cast<void *>(paLayer), NULL, COPC_UA_Handler().getInstance().onWrite};
	UA_StatusCode retVal = UA_Server_setVariableNode_valueCallback(mOPCUAServer, *paNodeId, callback);
	return retVal;
}


void COPC_UA_Handler::onWrite(void *pa_pvCtx, __attribute__((unused)) const UA_NodeId nodeid, const UA_Variant *data, __attribute__((unused)) const UA_NumericRange *range){

	CComLayer *layer = static_cast<CComLayer *>(pa_pvCtx);

	EComResponse retVal = layer->recvData(static_cast<const void *>(data), 0);	//TODO: add multidimensional data handling with 'range'.

	/* Handle return of receive data */
	if(e_ProcessDataOk == retVal){
		//only update data in item if data could be read
	}

	if(e_Nothing != retVal){
		getInstance().startNewEventChain(layer->getCommFB());
	}

}



bool COPC_UA_Handler::readBackDataPoint(__attribute__((unused)) const UA_Variant *paValue, __attribute__((unused)) CIEC_ANY &paDataPoint){
	bool retVal = true;
/*
	switch (paDataPoint.getDataTypeID()){
	case CIEC_ANY::e_BOOL:
		if(UA_TYPES_BOOLEAN == *(paValue->type)){
			((CIEC_BOOL &) paDataPoint) = paValue->data;
		}else{
			retVal = false;
		}
		break;
	case CIEC_ANY::e_SINT:
		if(UA_TYPES_SBYTE == *(paValue->type)){
			((CIEC_SINT &) paDataPoint) = (paValue->data);
		}else{
			retVal = false;
		}
		break;
	case CIEC_ANY::e_INT:
		if(UA_TYPES_INT16 == *(paValue->type)){
			((CIEC_INT &) paDataPoint) = (paValue->data);
		}else{
			retVal = false;
		}
		break;
	case CIEC_ANY::e_DINT:
		if(UA_TYPES_INT32 == *(paValue->type)){
			((CIEC_DINT &) paDataPoint) = (paValue->data);
		}else{
			retVal = false;
		}
		break;
#ifdef FORTE_USE_64BIT_DATATYPES
	case CIEC_ANY::e_LINT:
		if(UA_TYPES_INT64 == *(paValue->type)){
			((CIEC_LINT &) paDataPoint) = (paValue->data);
		}else{
			retVal = false;
		}
		break;
#endif
	case CIEC_ANY::e_USINT:
		if(UA_TYPES_BYTE == *(paValue->type)){
			((CIEC_USINT &) paDataPoint) = (paValue->data);
		}else{
			retVal = false;
		}
		break;
	case CIEC_ANY::e_UINT:
		if(UA_TYPES_UINT16 == *(paValue->type)){
			((CIEC_UINT &) paDataPoint) = (paValue->data);
		}else{
			retVal = false;
		}
		break;
	case CIEC_ANY::e_UDINT:
		if(UA_TYPES_UINT32 == *(paValue->type)){
			((CIEC_UDINT &) paDataPoint) = (paValue->data);
		}else{
			retVal = false;
		}
		break;
#ifdef FORTE_USE_64BIT_DATATYPES
	case CIEC_ANY::e_ULINT:
		if(UA_TYPES_UINT64 == *(paValue->type)){
			((CIEC_ULINT &) paDataPoint) = (paValue->data);
		}else{
			retVal = false;
		}
		break;
#endif
	case CIEC_ANY::e_BYTE:
		if(UA_TYPES_BYTE == *(paValue->type)){
			((CIEC_USINT &) paDataPoint) = (paValue->data);
		}else{
			retVal = false;
		}
		break;
	case CIEC_ANY::e_WORD:
		if(UA_TYPES_UINT16 == *(paValue->type)){
			((CIEC_WORD &) paDataPoint) = (paValue->data);
		}else{
			retVal = false;
		}
		break;
	case CIEC_ANY::e_DWORD:
		if(UA_TYPES_UINT32 == *(paValue->type)){
			((CIEC_USINT &) paDataPoint) = (paValue->data);
		}else{
			retVal = false;
		}
		break;
#ifdef FORTE_USE_64BIT_DATATYPES
	case CIEC_ANY::e_LWORD:
		if(UA_TYPES_UINT64 == *(paValue->type)){
			((CIEC_LWORD &) paDataPoint) = (paValue->data);
		}else{
			retVal = false;
		}
		break;
#endif
	case CIEC_ANY::e_REAL:
		if(UA_TYPES_FLOAT == *(paValue->type)){
			((CIEC_ANY_REAL &) paDataPoint) = (paValue->data);
		}else{
			retVal = false;
		}
		break;
#ifdef FORTE_USE_64BIT_DATATYPES
	case CIEC_ANY::e_LREAL:
		if(UA_TYPES_DOUBLE == *(paValue->type)){
			((CIEC_LREAL &) paDataPoint) = (paValue->data);
		}else{
			retVal = false;
		}
		break;
#endif
	case CIEC_ANY::e_STRING:
		if(UA_TYPES_STRING == *(paValue->type)){
			((CIEC_STRING &) paDataPoint) = (paValue->data);
		}else{
			retVal = false;
		}
		break;
#ifdef FORTE_USE_64BIT_DATATYPES

	case CIEC_ANY::e_WSTRING:
		if(UA_TYPES_STRING == *(paValue->type)){
		((CIEC_WSTRING &) paDataPoint) = (paValue->data);
	}else{
		retVal = false;
	}
	break;
#endif
	default:
		//TODO handle other datatypes
		retVal = false;
		break;
	}
*/
	return retVal;
}







