// Stub fecal.h - FEC disabled for Pico (not enough RAM)
// When FEC is needed, packets will be NACKed and retransmitted

#ifndef FECAL_H
#define FECAL_H

#include <stdint.h>
#include <stddef.h>

typedef void* FecalDecoder;

typedef struct {
    void* Data;
    size_t Bytes;
    int Index;
} FecalSymbol;

typedef struct {
    FecalSymbol* Symbols;
    int Count;
} RecoveredSymbols;

enum {
    Fecal_NeedMoreData = 1
};

static inline int fecal_init(void) { return 0; }
static inline FecalDecoder fecal_decoder_create(int, size_t) { return NULL; }
static inline int fecal_decoder_add_original(FecalDecoder, FecalSymbol*) { return -1; }
static inline int fecal_decoder_add_recovery(FecalDecoder, FecalSymbol*) { return -1; }
static inline int fecal_decode(FecalDecoder, RecoveredSymbols*) { return Fecal_NeedMoreData; }
static inline void fecal_free(FecalDecoder) {}

#endif // FECAL_H
