/*
 * SCSI sense code
 *
 * Copyright 2016 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef QEMU_SCSI_SENSE_H
#define QEMU_SCSI_SENSE_H

#define SCSI_SENSE_LEN         18
#define SCSI_SENSE_LEN_SCANNER 32

typedef struct SCSISense {
    uint8_t key;
    uint8_t asc;
    uint8_t ascq;
} SCSISense;

/*
 * Predefined sense codes
 */

/* No sense data available */
extern const struct SCSISense sense_code_NO_SENSE;
/* LUN not ready, Manual intervention required */
extern const struct SCSISense sense_code_LUN_NOT_READY;
/* LUN not ready, Medium not present */
extern const struct SCSISense sense_code_NO_MEDIUM;
/* LUN not ready, medium removal prevented */
extern const struct SCSISense sense_code_NOT_READY_REMOVAL_PREVENTED;
/* Hardware error, internal target failure */
extern const struct SCSISense sense_code_TARGET_FAILURE;
/* Illegal request, invalid command operation code */
extern const struct SCSISense sense_code_INVALID_OPCODE;
/* Illegal request, LBA out of range */
extern const struct SCSISense sense_code_LBA_OUT_OF_RANGE;
/* Illegal request, Invalid field in CDB */
extern const struct SCSISense sense_code_INVALID_FIELD;
/* Illegal request, Invalid field in parameter list */
extern const struct SCSISense sense_code_INVALID_PARAM;
/* Illegal request, Parameter list length error */
extern const struct SCSISense sense_code_INVALID_PARAM_LEN;
/* Illegal request, LUN not supported */
extern const struct SCSISense sense_code_LUN_NOT_SUPPORTED;
/* Illegal request, Saving parameters not supported */
extern const struct SCSISense sense_code_SAVING_PARAMS_NOT_SUPPORTED;
/* Illegal request, Incompatible format */
extern const struct SCSISense sense_code_INCOMPATIBLE_FORMAT;
/* Illegal request, medium removal prevented */
extern const struct SCSISense sense_code_ILLEGAL_REQ_REMOVAL_PREVENTED;
/* Illegal request, Invalid Transfer Tag */
extern const struct SCSISense sense_code_INVALID_TAG;
/* Command aborted, I/O process terminated */
extern const struct SCSISense sense_code_IO_ERROR;
/* Command aborted, I_T Nexus loss occurred */
extern const struct SCSISense sense_code_I_T_NEXUS_LOSS;
/* Command aborted, Logical Unit failure */
extern const struct SCSISense sense_code_LUN_FAILURE;
/* Command aborted, Overlapped Commands Attempted */
extern const struct SCSISense sense_code_OVERLAPPED_COMMANDS;
/* LUN not ready, Capacity data has changed */
extern const struct SCSISense sense_code_CAPACITY_CHANGED;
/* LUN not ready, Medium not present */
extern const struct SCSISense sense_code_UNIT_ATTENTION_NO_MEDIUM;
/* Unit attention, Power on, reset or bus device reset occurred */
extern const struct SCSISense sense_code_RESET;
/* Unit attention, Medium may have changed*/
extern const struct SCSISense sense_code_MEDIUM_CHANGED;
/* Unit attention, Reported LUNs data has changed */
extern const struct SCSISense sense_code_REPORTED_LUNS_CHANGED;
/* Unit attention, Device internal reset */
extern const struct SCSISense sense_code_DEVICE_INTERNAL_RESET;
/* Data Protection, Write Protected */
extern const struct SCSISense sense_code_WRITE_PROTECTED;
/* Data Protection, Space Allocation Failed Write Protect */
extern const struct SCSISense sense_code_SPACE_ALLOC_FAILED;

#define SENSE_CODE(x) sense_code_ ## x

int scsi_build_sense(uint8_t *in_buf, int in_len,
                     uint8_t *buf, int len, bool fixed);

#endif
