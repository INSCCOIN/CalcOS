#ifndef FAKE_HW_VREG_H
#define FAKE_HW_VREG_H
#include <stdint.h>
typedef int vreg_voltage;
#define VREG_VOLTAGE_DEFAULT 11
#define VREG_VOLTAGE_1_15 15
#define VREG_VOLTAGE_1_20 16
#define VREG_VOLTAGE_1_25 17
#define VREG_VOLTAGE_1_30 18
void vreg_set_voltage(vreg_voltage v);
#endif
