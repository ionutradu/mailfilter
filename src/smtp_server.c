#define _XOPEN_SOURCE 500
#define _GNU_SOURCE

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>

#include "smtp_server.h"
#include "smtp.h"
#include "base64.h"

static uint64_t key;
static const char *module = "server";

struct smtp_cmd_tree cmd_tree;

int smtp_cmd_register(const char *cmd, smtp_cmd_hdlr_t hdlr, int prio, int invokable)
{
	struct smtp_cmd_tree *node = &cmd_tree, *aux;
	struct smtp_cmd_hdlr_list *hlink;
	struct list_head *p;
	const char *c;

	for (c = cmd; *c != '\0'; c++) {
		assert_log(*c >= 'A' && *c <= 'Z', &__main_config);
		if (node->next[*c - 'A'] != NULL) {
			node = node->next[*c - 'A'];
			continue;
		}
		aux = malloc(sizeof(struct smtp_cmd_tree));
		assert_log(aux != NULL, &__main_config);
		memset(aux, 0, sizeof(struct smtp_cmd_tree));
		INIT_LIST_HEAD(&aux->hdlrs);
		node->next[*c - 'A'] = aux;
		node = aux;
	}

	list_for_each(p, &node->hdlrs) {
		if (list_entry(p, struct smtp_cmd_hdlr_list, lh)->prio > prio)
			break;
	}

	hlink = malloc(sizeof(struct smtp_cmd_hdlr_list));
	assert_log(hlink != NULL, &__main_config);
	hlink->hdlr = hdlr;
	hlink->prio = prio;
	hlink->invokable = invokable;

	list_add_tail(&hlink->lh, p);
	return 0;
}

struct smtp_cmd_tree *smtp_cmd_lookup(const char *cmd)
{
	struct smtp_cmd_tree *node = &cmd_tree;

	while (*cmd != '\0' && node != NULL) {
		char c = *cmd;
		if (c >= 'a' && c <= 'z')
			c -= 'a' - 'A';
		if (c < 'A' || c > 'Z')
			return NULL;
		node = node->next[c - 'A'];
		cmd++;
	}

	return node;
}

int smtp_server_response(FILE *f, int code, const char *message)
{
	char *buf = (char *)message, *c;

	while ((c = index(buf, '\n'))) {
		*c = 0;
		fprintf(f, "%d-%s\r\n", code, buf);
		*c = '\n';
		buf = c + 1;
	}

	if (fprintf(f, "%d %s\r\n", code, buf) >= 0) {
		fflush(f);
		return 0;
	}

	return -1;
}

int smtp_server_process(struct smtp_server_context *ctx, const char *cmd, const char *arg, FILE *stream)
{
	int schs, continue_session = 1;
	struct smtp_cmd_hdlr_list *hlink;

	do {
		/* Save ctx->node to local var, since it can be changed by a
		 * command handler *while* we are walking the handler list */
		struct smtp_cmd_tree *node = ctx->node;

		ctx->code = 0;
		ctx->message = ctx->prev_message = NULL;

		list_for_each_entry(hlink, &node->hdlrs, lh) {

			if (ctx->prev_message != NULL)
				free(ctx->prev_message);
			ctx->prev_code = ctx->code;
			ctx->prev_message = ctx->message;
			ctx->code = 0;
			ctx->message = NULL;

			schs = hlink->hdlr(ctx, cmd, arg, stream);

			if (ctx->code == -1) {
				ctx->code = ctx->prev_code;
				ctx->message = ctx->prev_message;
				ctx->prev_code = 0;
				ctx->prev_message = NULL;
			}
			if (schs == SCHS_ABORT || schs == SCHS_QUIT)
				continue_session = 0;
			if (schs == SCHS_BREAK || schs == SCHS_ABORT)
				break;
		}

		if (ctx->code) {
			smtp_server_response(stream, ctx->code, ctx->message);
		} else if (schs != SCHS_CHAIN && schs != SCHS_IGNORE) {
			smtp_server_response(stream, 451, "Internal server error");
			smtp_set_transaction_state(ctx, NULL, 451, "Internal server error");
		}
		if (ctx->message)
			free(ctx->message);
		if (ctx->prev_message)
			free(ctx->prev_message);
	} while (schs == SCHS_CHAIN);

	return continue_session;
}

int __smtp_server_run(struct smtp_server_context *ctx, FILE *stream)
{
	int continue_session;
	char buf[SMTP_COMMAND_MAX + 1];

	/* Command handling loop */
	do {
		int oversized = 0;
		char *c = &buf[0];
		size_t i, n;

		buf[SMTP_COMMAND_MAX] = '\n';
		if (fgets(buf, sizeof(buf), stream) == NULL)
			return -1;

		/* Handle oversized commands */
		while (buf[SMTP_COMMAND_MAX] != '\n') {
			oversized = 1;
			buf[SMTP_COMMAND_MAX] = '\n';
			if (fgets(buf, sizeof(buf), stream) == NULL)
				return -1;
		}
		if (oversized) {
			smtp_server_response(stream, 421, "Command too long");
			return -1;
		}

		/* Parse SMTP command */
		c += strspn(c, white);
		n = strcspn(c, white);
		ctx->node = &cmd_tree;
		for (i = 0; i < n; i++) {
			if (c[i] >= 'a' && c[i] <= 'z')
				c[i] -= 'a' - 'A';
			if (c[i] < 'A' || c[i] > 'Z')
				break;
			if (ctx->node->next[c[i] - 'A'] == NULL)
				break;
			ctx->node = ctx->node->next[c[i] - 'A'];
		}
		if (i < n || !n || list_empty(&ctx->node->hdlrs)) {
			smtp_server_response(stream, 500, "Command not implemented");
			continue;
		}

		/* Prepare argument */
		if (c[n] != '\0') {
			c[n] = '\0';
			n++;
		}

		/* Invoke all command handlers */
		continue_session = smtp_server_process(ctx, c, c + n, stream);
	} while (continue_session);

	return 0;
}

void smtp_path_init(struct smtp_path *path)
{
	memset(path, 0, sizeof(struct smtp_path));
	INIT_LIST_HEAD(&path->domains);
	INIT_LIST_HEAD(&path->mailbox.domain.lh);
}

void smtp_path_cleanup(struct smtp_path *path)
{
	struct smtp_domain *pos, *n;

	if (path->mailbox.local != NULL && path->mailbox.local != EMPTY_STRING)
		free(path->mailbox.local);
	if (path->mailbox.domain.domain != NULL)
		free(path->mailbox.domain.domain);
	list_for_each_entry_safe(pos, n, &path->domains, lh) {
		free(pos->domain);
		free(pos);
	}

	//FIXME ctx->hdrs
}

void smtp_server_context_init(struct smtp_server_context *ctx)
{
	int i;

	memset(ctx, 0, sizeof(struct smtp_server_context));
	smtp_path_init(&ctx->rpath);
	INIT_LIST_HEAD(&ctx->fpath);

	for (i = 0; i < SMTP_PRIV_HASH_SIZE; i++)
		INIT_LIST_HEAD(&ctx->priv_hash[i]);

	INIT_LIST_HEAD(&ctx->hdrs);
}

/**
 * Free resources and prepare for another SMTP transaction.
 *
 * This function is not only used at the end of the SMTP session, but
 * also by the default RSET handler.
 *
 * Besides closing all resources, this also leaves the context ready for
 * another SMTP session.
 */
void smtp_server_context_cleanup(struct smtp_server_context *ctx)
{
	struct smtp_path *path, *path_aux;

	smtp_path_cleanup(&ctx->rpath);
	smtp_path_init(&ctx->rpath);

	list_for_each_entry_safe(path, path_aux, &ctx->fpath, mailbox.domain.lh) {
		smtp_path_cleanup(path);
		free(path);
	}
	INIT_LIST_HEAD(&ctx->fpath);

	if (ctx->body.stream != NULL)
		fclose(ctx->body.stream);
	ctx->body.stream = NULL;

	if (ctx->body.path[0] != '\0')
		unlink(ctx->body.path);
	ctx->body.path[0] = '\0';

	ctx->transaction.state.code = 0;
	if (ctx->transaction.state.message)
		free(ctx->transaction.state.message);
	ctx->transaction.state.message = NULL;
	ctx->transaction.module = NULL;
}

int smtp_server_run(struct smtp_server_context *ctx, FILE *stream)
{
	int ret;

	/* Handle initial greeting */
	if ((ctx->node = smtp_cmd_lookup("INIT")) != NULL) {
		if (!smtp_server_process(ctx, NULL, NULL, stream) || !ctx->code)
			return 0;
	}

	ret = __smtp_server_run(ctx, stream);

	/* Give all modules the chance to clean up (possibly after a broken
	 * connection */
	if ((ctx->node = smtp_cmd_lookup("TERM")) != NULL) {
		if (!smtp_server_process(ctx, NULL, NULL, stream) || !ctx->code)
			return 0;
	}
	smtp_server_context_cleanup(ctx);

	return ret;
}

int smtp_path_parse_cmd(struct smtp_path *path, const char *arg, const char *word)
{
	char *trailing = arg;

	/* Look for passed-in word */
	arg += strspn(arg, white);
	if (strncasecmp(arg, word, strlen(word)))
		return 1;
	arg += strlen(word);

	/* Look for colon */
	arg += strspn(arg, white);
	if (*(arg++) != ':')
		return 1;

	/* Parse actual path */
	arg += strspn(arg, white);
	if (smtp_path_parse(path, arg, &trailing)) {
		smtp_path_cleanup(path);
		return 1;
	}
	if (trailing == arg)
		return 0;

	arg = trailing + strspn(trailing, white);
	if (*arg == '\0')
		return 0;
	// FIXME handle extra params, such as "SIZE=nnn"

	return 0;
}

int smtp_auth_login_parse_user(struct smtp_server_context *ctx, const char *arg)
{
	ctx->code = 334;
	if (arg) {
		ctx->auth_user = base64_dec(arg, strlen(arg), NULL);
		if (!ctx->auth_user) {
			ctx->code = 501;
			ctx->message = strdup("Cannot decode AUTH parameter");
			return SCHS_BREAK;
		}
		ctx->node = smtp_cmd_lookup("ALOP");
		ctx->message = base64_enc("Password:", strlen("Password:"));
	}
	else {
		ctx->node = smtp_cmd_lookup("ALOU");
		ctx->message = base64_enc("Username:", strlen("Username:"));
	}
	return SCHS_CHAIN;
}

int smtp_auth_login_parse_pw(struct smtp_server_context *ctx, const char *arg)
{
	ctx->auth_pw = base64_dec(arg, strlen(arg), NULL);
	if (!ctx->auth_pw) {
		ctx->code = 501;
		ctx->message = strdup("Cannot decode AUTH parameter");
		return SCHS_BREAK;
	}
	ctx->code = 250;
	return SCHS_OK;
}

int smtp_auth_plain_parse(struct smtp_server_context *ctx, const char *arg)
{
	char *auth_info, *p;
	int len;

	ctx->node = smtp_cmd_lookup("APLP");

	/* Parse (user, pw) from arg = base64(\0username\0password) */
	if (arg) {
		auth_info = base64_dec(arg, strlen(arg), &len);
		if (!auth_info) {
			ctx->code = 501;
			ctx->message = strdup("Cannot decode AUTH parameter");
			return SCHS_BREAK;
		}
		ctx->auth_user = strdup(auth_info + 1);
		p = auth_info + strlen(auth_info + 1) + 2;
		assert_mod_log(p - auth_info < len);
		ctx->auth_pw = strdup(p);
		free(auth_info);
		return SCHS_CHAIN;
	}

	/* Request the base64 encoded authentication string */
	ctx->code = 334;
	ctx->message = NULL;
	return SCHS_CHAIN;
}

int smtp_auth_unknown_parse(struct smtp_server_context *ctx, const char *arg)
{
	ctx->code = 504;
	ctx->message = strdup("AUTH mechanism not available");
	return SCHS_BREAK;
}

int smtp_hdlr_init(struct smtp_server_context *ctx, const char *cmd, const char *arg, FILE *stream)
{
	ctx->code = 220;
	ctx->message = strdup("Mindbit Mail Filter");
	return SCHS_OK;
}

int smtp_hdlr_auth(struct smtp_server_context *ctx, const char *cmd, const char *arg, FILE *stream)
{
	struct {
		const char *name;
		int (*parse)(struct smtp_server_context *ctx, const char *arg);
	} auth_types[] = {
		{ "LOGIN", smtp_auth_login_parse_user },
		{ "PLAIN", smtp_auth_plain_parse },
		{ NULL, NULL },
	};
	char *c, tmp;
	int i;

	if (ctx->auth_user) {
		ctx->code = 503;
		ctx->message = strdup("Already Authenticated");
		return SCHS_OK;
	}

	c = strrchr(arg, ' ');
	if (c) {
		tmp = *c;
		*c = 0;
		ctx->auth_type = strdup(arg);
		*c = tmp;
		c++;
	}
	else {
		c = arg;
		c[strcspn(c, "\r\n")] = 0;
		ctx->auth_type = strdup(arg);
		c = NULL;
	}

	for (i = 0; auth_types[i].name; i++)
		if (!strcasecmp(ctx->auth_type, auth_types[i].name))
			return auth_types[i].parse(ctx, c);
	return smtp_auth_unknown_parse(ctx, c);
}

int smtp_hdlr_alou(struct smtp_server_context *ctx, const char *cmd, const char *arg, FILE *stream)
{
	char buf[SMTP_COMMAND_MAX + 1];

	assert_mod_log(!ctx->auth_user);

	if (fgets(buf, sizeof(buf), stream) == NULL)
		return SCHS_BREAK;

	if (!strcmp(buf, "*\r\n")) {
		ctx->code = 501;
		ctx->message = strdup("AUTH aborted");
		return SCHS_BREAK;
	}

	return smtp_auth_login_parse_user(ctx, buf);
}

int smtp_hdlr_alop(struct smtp_server_context *ctx, const char *cmd, const char *arg, FILE *stream)
{
	char buf[SMTP_COMMAND_MAX + 1];

	assert_mod_log(!ctx->auth_pw);

	if (fgets(buf, sizeof(buf), stream) == NULL)
		return SCHS_BREAK;

	if (!strcmp(buf, "*\r\n")) {
		ctx->code = 501;
		ctx->message = strdup("AUTH aborted");
		return SCHS_BREAK;
	}

	return smtp_auth_login_parse_pw(ctx, buf);
}

int smtp_hdlr_aplp(struct smtp_server_context *ctx, const char *cmd, const char *arg, FILE *stream)
{
	char buf[SMTP_COMMAND_MAX + 1];

	if (!ctx->auth_user) {
		if (fgets(buf, sizeof(buf), stream) == NULL)
			return SCHS_BREAK;

		return smtp_auth_plain_parse(ctx, buf);
	}
	return SCHS_OK;
}

int smtp_hdlr_ehlo(struct smtp_server_context *ctx, const char *cmd, const char *arg, FILE *stream)
{
	char *domain;

	/* We must break the rules and modify arg to strip the terminating newline. Otherwise
	 * the server to which we're proxying gets confused, since it expects the \r\n line
	 * ending. smtp_client_command already appends this.
	 */
	domain = (char *)arg;
	domain[strcspn(domain, "\r\n")] = '\0';

	/* Store client identity in the server's context */
	ctx->identity = strdup(domain);
	ctx->code = 250;
	ctx->message = strdup("AUTH LOGIN PLAIN\nHELP");

	return SCHS_OK;
}

int smtp_hdlr_mail(struct smtp_server_context *ctx, const char *cmd, const char *arg, FILE *stream)
{
	if (ctx->rpath.mailbox.local != NULL) {
		ctx->code = 503;
		ctx->message = strdup("Sender already specified");
		return SCHS_BREAK;
	}

	if (smtp_path_parse_cmd(&ctx->rpath, arg, "FROM")) {
		smtp_path_init(&ctx->rpath);
		ctx->code = 501;
		ctx->message = strdup("Syntax error");
		return SCHS_BREAK;
	}

	ctx->code = 250;
	ctx->message = strdup("Envelope sender ok");
	return SCHS_OK;
}

int smtp_hdlr_rcpt(struct smtp_server_context *ctx, const char *cmd, const char *arg, FILE *stream)
{
	struct smtp_path *path;

	if (ctx->rpath.mailbox.local == NULL) {
		ctx->code = 503;
		ctx->message = strdup("Must specify envelope sender first");
		return SCHS_BREAK;
	}

	path = malloc(sizeof(struct smtp_path));
	if (path == NULL)
		return SCHS_BREAK;
	smtp_path_init(path);

	if (smtp_path_parse_cmd(path, arg, "TO")) {
		free(path);
		ctx->code = 501;
		ctx->message = strdup("Syntax error");
		return SCHS_BREAK;
	}

	list_add_tail(&path->mailbox.domain.lh, &ctx->fpath);
	ctx->code = 250;
	ctx->message = strdup("Recipient ok");

	return SCHS_OK;
}

int smtp_hdlr_data(struct smtp_server_context *ctx, const char *cmd, const char *arg, FILE *stream)
{
	int fd;

	// TODO verificare existenta envelope sender si recipienti; salvare mail in temporar; copiere path temp in smtp_server_context
	if (list_empty(&ctx->fpath)) {
		ctx->code = 503;
		ctx->message = strdup("Must specify recipient(s) first");
		return SCHS_BREAK;
	}

	/* prepare temporary file to store message body */
	sprintf(ctx->body.path, "/tmp/mailfilter.XXXXXX"); // FIXME sNprintf; cale in loc de /tmp;
	if ((fd = mkstemp(ctx->body.path)) == -1) {
		ctx->body.path[0] = '\0';
		return SCHS_BREAK;
	}
	if ((ctx->body.stream = fdopen(fd, "r+")) == NULL) {
		close(fd);
		unlink(ctx->body.path);
		ctx->body.path[0] = '\0';
		return SCHS_BREAK;
	}

	/* prepare response */
	ctx->code = 354;
	ctx->message = strdup("Go ahead");
	ctx->node = smtp_cmd_lookup("BODY");
	return SCHS_CHAIN;
}

int smtp_copy_to_file(FILE *out, FILE *in, struct im_header_context *im_hdr_ctx)
{
	const uint64_t DOTLINE_MAGIC	= 0x0d0a2e0000;	/* <CR><LF>"."<*> */
	const uint64_t DOTLINE_MASK		= 0xffffff0000;
	const uint64_t CRLF_MAGIC		= 0x0000000d0a; /* <CR><LF> */
	const uint64_t CRLF_MASK		= 0x000000ffff;
	uint64_t buf = 0;
	int fill = 0;
	int im_state = IM_OK;
	int c;

	while ((c = getc_unlocked(in)) != EOF) {
		if (im_hdr_ctx && im_state == IM_OK) {
			im_state = im_header_feed(im_hdr_ctx, c);
			continue;
		}
		if (++fill > 8) {
			if (putc_unlocked(buf >> 56, out) == EOF)
				return -EIO;
			fill = 8;
		}
		buf = (buf << 8) | c;
		if ((buf & DOTLINE_MASK) != DOTLINE_MAGIC)
			continue;
		if ((buf & CRLF_MASK) == CRLF_MAGIC) {
			/* we found the EOF sequence (<CR><LF>"."<CR><LF>) */
			assert_log(fill >= 5, &__main_config);
			/* discard the (terminating) "."<CR><LF> */
			buf >>= 24;
			fill -= 3;
			break;
		}
		/* strip the dot at beginning of line */
		assert_log(fill >= 5, &__main_config);
		buf = ((buf >> 8) & ~CRLF_MASK) | (buf & CRLF_MASK);
		fill--;
	}

	/* flush remaining buffer */
	for (fill = (fill - 1) * 8; fill >= 0; fill -= 8)
		if (putc_unlocked((buf >> fill) & 0xff, out) == EOF)
			return -EIO;

	return im_state == IM_OK || im_state == IM_COMPLETE ? 0 : im_state;
}

int smtp_hdlr_body(struct smtp_server_context *ctx, const char *cmd, const char *arg, FILE *stream)
{
	struct im_header_context im_hdr_ctx = IM_HEADER_CONTEXT_INITIALIZER;

	assert_mod_log(ctx->body.stream != NULL);

	im_hdr_ctx.max_size = 65536; // FIXME use proper value
	im_hdr_ctx.hdrs = &ctx->hdrs;
	//sleep(10);
	switch (smtp_copy_to_file(ctx->body.stream, stream, &im_hdr_ctx)) {
	case 0:
		ctx->code = 250;
		ctx->message = strdup("Mail successfully received");
		break;
	case IM_PARSE_ERROR:
		ctx->code = 500;
		ctx->message = strdup("Could not parse message headers");
		break;
	case IM_OVERRUN:
		ctx->code = 552;
		ctx->message = strdup("Message header size exceeds safety limits");
		break;
	default:
		ctx->code = 452;
		ctx->message = strdup("Insufficient system storage");
	}
	fflush(ctx->body.stream);
	smtp_set_transaction_state(ctx, module, 0, NULL);
	//im_header_write(&ctx->hdrs, stdout);
	return SCHS_OK;
}

int smtp_hdlr_quit(struct smtp_server_context *ctx, const char *cmd, const char *arg, FILE *stream)
{
	ctx->code = 221;
	ctx->message = strdup("closing connection");
	return SCHS_QUIT;
}

int smtp_hdlr_rset(struct smtp_server_context *ctx, const char *cmd, const char *arg, FILE *stream)
{
	smtp_server_context_cleanup(ctx);
	ctx->code = 250;
	ctx->message = strdup("State reset complete");
	return SCHS_OK;
}

void smtp_server_init(void)
{
	memset(&cmd_tree, 0, sizeof(struct smtp_cmd_tree));
	INIT_LIST_HEAD(&cmd_tree.hdlrs);
	smtp_cmd_register("INIT", smtp_hdlr_init, 0, 0);
	smtp_cmd_register("AUTH", smtp_hdlr_auth, 0, 1);
	smtp_cmd_register("ALOU", smtp_hdlr_alou, 0, 0);
	smtp_cmd_register("ALOP", smtp_hdlr_alop, 0, 0);
	smtp_cmd_register("APLP", smtp_hdlr_aplp, 0, 0);
	smtp_cmd_register("EHLO", smtp_hdlr_ehlo, 0, 1);
	smtp_cmd_register("MAIL", smtp_hdlr_mail, 0, 1);
	smtp_cmd_register("RCPT", smtp_hdlr_rcpt, 0, 1);
	smtp_cmd_register("DATA", smtp_hdlr_data, 0, 1);
	smtp_cmd_register("BODY", smtp_hdlr_body, 0, 0);
	smtp_cmd_register("QUIT", smtp_hdlr_quit, 0, 1);
	smtp_cmd_register("RSET", smtp_hdlr_rset, 0, 1);

	// TODO urmatoarele trebuie sa se intample din config
	mod_proxy_init();
	mod_spamassassin_init();
	mod_clamav_init();
	mod_log_sql_init();
}

int smtp_priv_register(struct smtp_server_context *ctx, uint64_t key, void *priv)
{
	struct smtp_priv_hash *h;

	h = malloc(sizeof(struct smtp_priv_hash));
	if (h == NULL)
		return -ENOMEM;

	h->key = key;
	h->priv = priv;
	list_add_tail(&h->lh, &ctx->priv_hash[smtp_priv_bucket(key)]);

	return 0;
}

void *smtp_priv_lookup(struct smtp_server_context *ctx, uint64_t key)
{
	struct smtp_priv_hash *h;
	int i = smtp_priv_bucket(key);

	list_for_each_entry(h, &ctx->priv_hash[i], lh)
		if (h->key == key)
			return h->priv;

	return NULL;
}

int smtp_priv_unregister(struct smtp_server_context *ctx, uint64_t key)
{
	struct smtp_priv_hash *h;
	int i = smtp_priv_bucket(key);

	list_for_each_entry(h, &ctx->priv_hash[i], lh)
		if (h->key == key) {
			list_del(&h->lh);
			free(h);
			return 0;
		}

	return -ESRCH;
}

int smtp_set_transaction_state(struct smtp_server_context *ctx, const char *__module, int code, const char *message)
{
	char *__message;
	
	/* default param values */
	if (!code)
		code = ctx->code;

	if (!message)
		message = ctx->message;

	/* update ctx->transaction */
	if (message) {
		__message = strdup(message);
		if (__message == NULL)
			return -ENOMEM;
		if (ctx->transaction.state.message)
			free(ctx->transaction.state.message);
		ctx->transaction.state.message = __message;
	}

	ctx->transaction.state.code = code;

	if (__module)
		ctx->transaction.module = __module;

	return 0;
}

/* FIXME move this to some lib/util file */
int stream_copy(FILE *src, FILE *dst)
{
	char buf[4096];
	size_t sz;

	while ((sz = fread(buf, 1, sizeof(buf), src)))
		if (!fwrite(buf, sz, 1, dst))
			return -1;

	return !feof(src);
}
