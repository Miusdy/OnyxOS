#include "common/auth.h"
int account_load(struct accounts *);
int account_find(struct accounts *, const char *);
int account_line(char *, int);
int account_password(const char *, char *, int);
