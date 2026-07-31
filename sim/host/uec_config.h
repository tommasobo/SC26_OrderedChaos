#pragma once
#define INIT_PULL \
    100000000  // needs to be large enough we don't map
               // negative pull targets (where
               // credit_spec > backlog) to less than
               // zero and suffer underflow.  Real
               // implementations will properly handle
               // modular wrapping.

static const unsigned uecMaxInFlightPkts = 1 << 16;
