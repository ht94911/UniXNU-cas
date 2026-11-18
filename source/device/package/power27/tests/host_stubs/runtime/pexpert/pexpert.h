#pragma once
#ifdef NP_HOST_BOOT_ARGUMENT
int PE_parse_boot_argn(const char *,void *,int);
#else
inline int PE_parse_boot_argn(const char *,void *,int){return 0;}
#endif
