#ifndef EIGRP_TEST_COMMAND_H
#define EIGRP_TEST_COMMAND_H
#include <zebra.h>
#define DEFUN_CMD_ELEMENT(funcname, cmdname, cmdstr, helpstr, attrs, dnum) \
	const struct cmd_element cmdname = { .string = cmdstr, .func = funcname, .doc = helpstr, .attr = attrs, .daemon = dnum, .name = #cmdname, .xref = NULL };
#define DEFUN_CMD_FUNC_DECL(funcname) static int funcname(const struct cmd_element *, struct vty *, int, struct cmd_token *[])
#define DEFUN_CMD_FUNC_TEXT(funcname) static int funcname(const struct cmd_element *self __attribute__((unused)), struct vty *vty __attribute__((unused)), int argc __attribute__((unused)), struct cmd_token *argv[] __attribute__((unused)))
#define DEFUN_ATTR(funcname, cmdname, cmdstr, helpstr, attr) DEFUN_CMD_FUNC_DECL(funcname); static DEFUN_CMD_ELEMENT(funcname, cmdname, cmdstr, helpstr, attr, 0) DEFUN_CMD_FUNC_TEXT(funcname)
#define DEFUN(funcname, cmdname, cmdstr, helpstr) DEFUN_ATTR(funcname, cmdname, cmdstr, helpstr, 0)
#define DEFUN_HIDDEN(funcname, cmdname, cmdstr, helpstr) DEFUN_ATTR(funcname, cmdname, cmdstr, helpstr, CMD_ATTR_HIDDEN)
#define DEFUN_NOSH(funcname, cmdname, cmdstr, helpstr) DEFUN_ATTR(funcname, cmdname, cmdstr, helpstr, CMD_ATTR_NOSH)
#define DEFUN_YANG(funcname, cmdname, cmdstr, helpstr) DEFUN_ATTR(funcname, cmdname, cmdstr, helpstr, CMD_ATTR_YANG)
#define DEFPY(funcname, cmdname, cmdstr, helpstr) DEFUN(funcname, cmdname, cmdstr, helpstr)
#define DEFPY_NOSH(funcname, cmdname, cmdstr, helpstr) DEFUN_NOSH(funcname, cmdname, cmdstr, helpstr)
#define DEFPY_YANG(funcname, cmdname, cmdstr, helpstr) DEFUN_YANG(funcname, cmdname, cmdstr, helpstr)
#define DEFPY_YANG_NOSH(funcname, cmdname, cmdstr, helpstr) DEFUN_ATTR(funcname, cmdname, cmdstr, helpstr, CMD_ATTR_YANG|CMD_ATTR_NOSH)
#define DEFPY_HIDDEN(funcname, cmdname, cmdstr, helpstr) DEFUN_HIDDEN(funcname, cmdname, cmdstr, helpstr)
#define ALIAS(funcname, cmdname, cmdstr, helpstr) static DEFUN_CMD_ELEMENT(funcname, cmdname, cmdstr, helpstr, 0, 0)
#endif
