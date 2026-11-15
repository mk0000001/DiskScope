#ifndef DS_CLI_H
#define DS_CLI_H

#include "diskscope/ds_snapshot.h"

int ds_cli_scan(int argc, char **argv);
int ds_cli_export(int argc, char **argv);
int ds_cli_compare(int argc, char **argv);
int ds_cli_result(DsStatus status, const DsError *error);
void ds_cli_interrupt(void);
bool ds_cli_interrupted(void);
unsigned ds_cli_interrupt_count(void);

#endif
