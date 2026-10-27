// Tools/prepare-native-stack.py expands this empty placeholder after linking and before signing.
// Keeping it in the linked image gives chained fixups the correct segment count without moving __LINKEDIT.
.zerofill __LSB_STACK,__reserve,_LSBStackReservation,0x4000,14
.private_extern _LSBStackReservation
.no_dead_strip _LSBStackReservation
