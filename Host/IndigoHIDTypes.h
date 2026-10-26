#pragma once

// Indigo names the edge where a contact began, not its direction of travel.
typedef enum {
    LSBIndigoTouchEdgeNone = 0,
    LSBIndigoTouchEdgeTop = 1,
    LSBIndigoTouchEdgeLeft = 2,
    LSBIndigoTouchEdgeBottom = 3,
    LSBIndigoTouchEdgeRight = 4
} LSBIndigoTouchEdge;
