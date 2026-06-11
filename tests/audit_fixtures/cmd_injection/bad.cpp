// Fixture: cmd_injection rule.
#include <cstdlib>
#include <cstdio>
#include <unistd.h>

void danger1(const char *user) { (void)!system(user); }                           // @expect cmd_injection
void danger2(const char *cmd)  { (void)popen(cmd, "r"); }                         // @expect cmd_injection
void danger3()                 { (void)execl("/bin/sh", "sh", "-c", "x", nullptr); } // @expect cmd_injection
void danger4(const char *cmd)  { (void)execlp("sh", "sh", "-c", cmd, nullptr); }  // @expect cmd_injection
void danger5(char *const v[])  { (void)execvp("sh", v); }                          // @expect cmd_injection
void danger6(char *const v[])  { (void)execv("/bin/sh", v); }                      // @expect cmd_injection
void danger7(char *const e[])  { (void)execle("/bin/sh", "sh", nullptr, e); }      // @expect cmd_injection
