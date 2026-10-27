#ifndef LSB_STACK_RESERVATION_H
#define LSB_STACK_RESERVATION_H

#define LSB_STACK_RESERVATION_SEGMENT "__LSB_STACK"
#define LSB_STACK_RESERVATION_SECTION "__reserve"
#define LSB_STACK_RESERVATION_ADDRESS 0x10b000000ULL
#define LSB_STACK_RESERVATION_SIZE 0x65000000ULL
#define LSB_STACK_RESERVATION_MAX_SLIDE 0x5000000ULL

// arm64 XNU: USRSTACK64 is 0x16fe00000, reduced by the executable's ASLR slide (less than 80 MiB).
// This empty segment moves upward by that slide, covering the whole native stack window while ending below 0x175000000.
// create_unix_stack then allocates elsewhere and updates p->user_stack itself; LC_MAIN and native argument storage survive.
// https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/bsd/kern/kern_exec.c#L6680

#endif
