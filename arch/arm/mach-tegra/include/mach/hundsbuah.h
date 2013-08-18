
#ifndef _HUNDSBUAH_H_
#define _HUNDSBUAH_H_

/* TF201
 * SOCID = 2
 * CPUID = 3
 */

/* TF700T
 * SOCID = 2
 * CPUID = 5
 */

/* Tegra3 TF700/T OC */

/*****************************/
/*                           */
/*      Voltage section      */
/*                           */
/*****************************/

/* cap voltages */
#define HUNDSBUAH_TF700T_CPU_VOLTAGE_CAP         (1387)   /* mV */
#define HUNDSBUAH_TF700T_CORE_VOLTAGE_CAP        (1425)   /* mV */

/* max and min voltages */
#define HUNDSBUAH_TF700T_MIN_CPU_VOLTAGE         (600)    /* mV */
#define HUNDSBUAH_TF700T_MAX_CPU_VOLTAGE         (1387)   /* mV */

#define HUNDSBUAH_TF700T_MIN_CORE_VOLTAGE        (850)    /* mV */
#define HUNDSBUAH_TF700T_MAX_CORE_VOLTAGE        (1425)   /* mV */


/*****************************/
/*                           */
/*    Frequency section      */
/*                           */
/*****************************/
/* lp core */
#define HUNDSBUAH_CPU_LP_FREQUENCY_CAP    (620)    /* MHz */

/* cap frequencys */
#define HUNDSBUAH_CPU_FREQUENCY_CAP       (1900)   /* MHz */
#define HUNDSBUAH_CORE_FREQUENCY_CAP      (700)    /* MHz */
#define HUNDSBUAH_HOST1X_FREQUENCY_CAP    (350)    /* MHz */

/* max. frequencys */
#define HUNDSBUAH_TF700T_MAX_CPU_FREQUENCY       (1900)   /* MHz */
#define HUNDSBUAH_TF700T_MAX_CORE_FREQUENCY      (700)    /* MHz */
#define HUNDSBUAH_TF700T_MAX_HOST1X_FREQUENCY    (350)    /* MHz */
#define HUNDSBUAH_TF201_MAX_CORE_FREQUENCY       (700)    /* MHz */


#define HUNDSBUAH_SYSTEM_NORMAL_MODE_FREQUENCY     (HUNDSBUAH_TF700T_MAX_CPU_FREQUENCY * 1000)    /* kHz */
#define HUNDSBUAH_SYSTEM_BALANCE_MODE_FREQUENCY    (HUNDSBUAH_TF700T_MAX_CPU_FREQUENCY * 1000)    /* kHz */
#define HUNDSBUAH_SYSTEM_PWRSAVE_MODE_FREQUENCY    (HUNDSBUAH_TF700T_MAX_CPU_FREQUENCY * 1000)    /* kHz */

/* Limit max. cpu speed until booting is finished.
 * This has been added because some devices cant handle frequencys above
 * 1850MHz (but i want to provide them) and when the cpu gets initialized, its running a short time at
 * max frequency until booting is finished and the cpuX.sh file is executed.
 * This can cause a freeze if the tablet cant handle these high frequencys.
 * Therefore the boot frequency is capped until the boot is completed.
 * The cpuX.sh file is now responsible to set the correct max. cpu frequency cap
 * for your tablet and notify the cpu via boot_completed module parameter
 * "echo 1 > /sys/module/cpu_tegra/parameters/boot_finished" to unlock the
 * highest cpu frequencys. Now the tablet wont go higher than the frequency set with:
 * "echo 760000 > /sys/module/cpu_tegra/parameters/pwr_cap_limit_1"
 * and frequencys above 1850 are still possible to other tablet users who can handle
 * high frequencys.
 * Just to be sure that boot_finished is set after pwr_cap_limit_X was set!
 */
#define HUNDSBUAH_CPU_BOOT_FREQUENCY_TF700T         (1500000) /* kHz */
#define HUNDSBUAH_CPU_BOOT_FREQUENCY_TF201          (1200000) /* kHz */
#define HUNDSBUAH_CPU_BOOT_FREQUENCY_DEFAULT        (1000000) /* kHz */
#endif
