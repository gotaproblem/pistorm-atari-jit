/* input_script.h - PISTORM_INPUT_SCRIPT, see input_script.c */
#ifndef INPUT_SCRIPT_H
#define INPUT_SCRIPT_H
#ifdef __cplusplus
extern "C" {
#endif
int  input_script_wanted(void);           /* the variable is set           */
int  input_script_init(void);             /* after the injection layer     */
int  input_script_armed(void);            /* waiting for the start Pexec   */
void input_script_pexec(const char *name);/* GEMDOS Pexec, CPU thread      */
#ifdef __cplusplus
}
#endif
#endif
