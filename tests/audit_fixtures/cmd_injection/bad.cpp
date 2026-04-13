// Fixture: cmd_injection rule.
#include <cstdlib>
#include <cstdio>
#include <unistd.h>

void danger1(const char *user) { (void)!system(user); }                           // @expect cmd_injection
void danger2(const char *cmd)  { (void)popen(cmd, "r"); }                         // @expect cmd_injection
void danger3()                 { (void)execl("/bin/sh", "sh", "-c", "x", nullptr); } // @expect cmd_injection
