#include "smtpserver.h"

DEFINE_HANDLER_STUB(Init);
DEFINE_HANDLER_STUB(Auth);
DEFINE_HANDLER_STUB(Alou);
DEFINE_HANDLER_STUB(Alop);
DEFINE_HANDLER_STUB(Ehlo);
DEFINE_HANDLER_STUB(Data);
DEFINE_HANDLER_STUB(Mail);
DEFINE_HANDLER_STUB(Rcpt);
DEFINE_HANDLER_STUB(Rset);
DEFINE_HANDLER_STUB(Body);
DEFINE_HANDLER_STUB(Clnp);

jsval create_response(JSContext *cx, int code, const char* message, int disconnect) { 
	jsval rmessage;
	JSObject *obj;
	
	obj = JS_NewObject(cx, NULL, NULL, NULL);
	
	if (message != NULL) {
		rmessage = STRING_TO_JSVAL(JS_InternString(cx, message));
	} else {
		// TODO
		// define message property with default value for current code
		rmessage = STRING_TO_JSVAL(JS_InternString(cx, "default err message"));
	}
	
	JS_DefineProperty(cx, obj, "code", INT_TO_JSVAL(code), NULL, NULL, JSPROP_ENUMERATE | JSPROP_READONLY | JSPROP_PERMANENT);
	
	JS_DefineProperty(cx, obj, "message", rmessage, NULL, NULL, JSPROP_ENUMERATE | JSPROP_READONLY | JSPROP_PERMANENT);
	
	JS_DefineProperty(cx, obj, "disconnect", INT_TO_JSVAL(disconnect), NULL, NULL, JSPROP_ENUMERATE | JSPROP_READONLY | JSPROP_PERMANENT);
	
	return OBJECT_TO_JSVAL(obj);
}

static JSBool smtpPath_construct(JSContext *cx, unsigned argc, jsval *vp) {
	js_dump(cx, argc, vp);
	printf("smtpPath constructor()\n");
	return JS_TRUE;
}

static JSBool smtpPath_toString(JSContext *cx, unsigned argc, jsval *vp) {
	js_dump(cx, argc, vp);
	printf("smtpPath toString()\n");
	return JS_TRUE;
}

int init_smtp_path_class(JSContext *cx, JSObject *global) {
	static JSClass smtpPath_class = {
	    "SmtpPath", 0,
	    JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_PropertyStub,
	    JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, JS_PropertyStub,
	    NULL, NULL, NULL, smtpPath_construct, NULL, NULL, NULL, NULL
	};

	// Create the SmtpPath class
	JSObject *smtpPathClass = JS_InitClass(cx, global, NULL, &smtpPath_class, smtpPath_construct, 1, NULL, NULL, NULL, NULL);

	if (!smtpPathClass) {
		return -1;
	}

	JSObject *proto = JS_GetObjectPrototype(cx, smtpPathClass);

	// Add domains property
	JSObject *domains = JS_NewArrayObject(cx, 0, NULL);

	if (!domains) {
		return -1;
	}

	if (!JS_DefineProperty(cx, proto, "domains", OBJECT_TO_JSVAL(domains), NULL, NULL, JSPROP_ENUMERATE | JSPROP_READONLY | JSPROP_PERMANENT)) {
		return -1;
	}

	// Add mailbox property
	JSObject *mailbox = JS_NewObject(cx, NULL, NULL, NULL);

	if (!mailbox) {
		return -1;
	}

	if (!JS_DefineProperty(cx, mailbox, "local", STRING_TO_JSVAL(JS_InternString(cx, "")), NULL, NULL, JSPROP_ENUMERATE | JSPROP_READONLY | JSPROP_PERMANENT)) {
		return -1;
	}

	if (!JS_DefineProperty(cx, mailbox, "domain", STRING_TO_JSVAL(JS_InternString(cx, "")), NULL, NULL, JSPROP_ENUMERATE | JSPROP_READONLY | JSPROP_PERMANENT)) {
		return -1;
	}

	if (!JS_DefineProperty(cx, proto, "mailbox", OBJECT_TO_JSVAL(mailbox), NULL, NULL, JSPROP_ENUMERATE | JSPROP_READONLY | JSPROP_PERMANENT)) {
		return -1;
	}

	if (!JS_DefineFunction(cx, proto, "toString", smtpPath_toString, 0, 0)) {
		return -1;
	}

	return 0;
}

int js_smtp_server_obj_init(JSContext *cx, JSObject *global)
{
	static JSClass smtpserver_class = {
		"smtpServer", 0, JS_PropertyStub, JS_PropertyStub,
		JS_PropertyStub, JS_StrictPropertyStub, JS_EnumerateStub,
		JS_ResolveStub, JS_ConvertStub, JS_PropertyStub,
		JSCLASS_NO_OPTIONAL_MEMBERS
	};

	JSObject *smtpServer;

	smtpServer = JS_DefineObject(cx, global, "smtpServer", &smtpserver_class, NULL, 0);
	if (!smtpServer)
		return -1;

	JSFunctionSpec smtp_command_handlers[] = {
		JS_FS("smtpInit", smtpInit, 0, 0),
		JS_FS("smtpAlou", smtpAlou, 0, 0),
		JS_FS("smtpAlop", smtpAlop, 0, 0),
		JS_FS("smtpEhlo", smtpEhlo, 0, 0),
		JS_FS("smtpData", smtpData, 0, 0),
		JS_FS("smtpMail", smtpMail, 0, 0),
		JS_FS("smtpRcpt", smtpRcpt, 0, 0),
		JS_FS("smtpRset", smtpRset, 0, 0),
		JS_FS("smtpBody", smtpBody, 0, 0),
		JS_FS("smtpClnp", smtpClnp, 0, 0),
		JS_FS_END
	};

	if (JS_DefineFunctions(cx, smtpServer, smtp_command_handlers) == JS_FALSE) {
		return -1;
	}

	// Create session object (property of smtpServer)
	JSObject *session;
	session = JS_NewObject(cx, NULL, NULL, NULL);

	// Define and set session.quitAsserted = false
	if (JS_DefineProperty(cx, session, "quitAsserted", BOOLEAN_TO_JSVAL(JS_FALSE), NULL, NULL, JSPROP_ENUMERATE) == JS_FALSE) {
		return -1;
	}

	// Define smtpServer.session
	if (JS_DefineProperty(cx, smtpServer, "session", OBJECT_TO_JSVAL(session), NULL, NULL, JSPROP_ENUMERATE) == JS_FALSE) {
		return -1;
	}

	return 0;
}

