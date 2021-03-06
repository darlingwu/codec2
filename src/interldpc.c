/*---------------------------------------------------------------------------*\

  FILE........: interldpc.c
  AUTHOR......: David Rowe
  DATE CREATED: April 2018

  Helper functions for LDPC waveforms.

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2018 David Rowe

  All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License version 2.1, as
  published by the Free Software Foundation.  This program is
  distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or
  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
  License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "interldpc.h"
#include "ofdm_internal.h"
#include "mpdecode_core.h"
#include "gp_interleaver.h"

void set_up_ldpc_constants(struct LDPC *ldpc, int code_length, int parity_bits) {
    /* following provided for convenience and to match Octave variable names */

    /* these remain fixed */
    ldpc->ldpc_data_bits_per_frame = code_length - parity_bits;
    ldpc->ldpc_coded_bits_per_frame = code_length;

    /* in the case there are some unused data bits, these may be
       modified to be less that ldpc->ldpc_xxx versions above. We
       place known bits in the unused data bit positions, which make
       the code stronger, and allow us to mess with different speech
       codec bit allocations without designing new LDPC codes. */
    
    ldpc->data_bits_per_frame = ldpc->ldpc_data_bits_per_frame;
    ldpc->coded_bits_per_frame = ldpc->ldpc_coded_bits_per_frame;
}

void set_data_bits_per_frame(struct LDPC *ldpc, int new_data_bits_per_frame) {
    ldpc->data_bits_per_frame = new_data_bits_per_frame;
    ldpc->coded_bits_per_frame = ldpc->data_bits_per_frame + ldpc->NumberParityBits;
}
    
void ldpc_encode_frame(struct LDPC *ldpc, int codeword[], unsigned char tx_bits_char[]) {
    unsigned char pbits[ldpc->NumberParityBits];
    int i, j;

    if (ldpc->data_bits_per_frame == ldpc->ldpc_data_bits_per_frame) {
        /* we have enough data bits to fill the codeword */
        encode(ldpc, tx_bits_char, pbits);
    } else {        
        unsigned char tx_bits_char_padded[ldpc->ldpc_data_bits_per_frame];
        /* some unused data bits, set these to known values to strengthen code */    
        memcpy(tx_bits_char_padded, tx_bits_char, ldpc->data_bits_per_frame);
        for (i = ldpc->data_bits_per_frame; i < ldpc->ldpc_data_bits_per_frame; i++)
            tx_bits_char_padded[i] = 1;
        encode(ldpc, tx_bits_char_padded, pbits);
    }
          
    /* output codeword is concatenation of (used) data bits and parity
       bits, we don't bother sending unused (known) data bits */
    for (i = 0; i < ldpc->data_bits_per_frame; i++) {
        codeword[i] = tx_bits_char[i];
    }
    for (j = 0; j < ldpc->NumberParityBits; i++, j++) {
        codeword[i] = pbits[j];
    }
}

void qpsk_modulate_frame(COMP tx_symbols[], int codeword[], int n) {
    int s, i;
    int dibit[2];
    complex float qpsk_symb;

    for (s = 0, i = 0; i < n; s += 2, i++) {
        dibit[0] = codeword[s + 1] & 0x1;
        dibit[1] = codeword[s] & 0x1;
        qpsk_symb = qpsk_mod(dibit);
        tx_symbols[i].real = crealf(qpsk_symb);
        tx_symbols[i].imag = cimagf(qpsk_symb);
    }
}

/* Count uncoded (raw) bit errors over frame, note we don't include UW
   of txt bits as this is done after we dissassemmble the frame */

int count_uncoded_errors(struct LDPC *ldpc, struct OFDM_CONFIG *config, int *Nerrs_raw, COMP codeword_symbols_de[]) {
    int i, Nerrs, Terrs;

    int coded_syms_per_frame = ldpc->coded_bits_per_frame/config->bps;
    int coded_bits_per_frame = ldpc->coded_bits_per_frame;
    int data_bits_per_frame = ldpc->data_bits_per_frame;
    int rx_bits_raw[coded_bits_per_frame];

    /* generate test codeword from known payload data bits */

    int test_codeword[coded_bits_per_frame];
    uint16_t r[data_bits_per_frame];
    uint8_t tx_bits[data_bits_per_frame];

    ofdm_rand(r, data_bits_per_frame);
    
    for (i = 0; i < data_bits_per_frame; i++) {
        tx_bits[i] = r[i] > 16384;
    }
    
    ldpc_encode_frame(ldpc, test_codeword, tx_bits);

    Terrs = 0;

    for (i = 0; i < coded_syms_per_frame; i++) {
        int bits[2];
        complex float s = codeword_symbols_de[i].real + I * codeword_symbols_de[i].imag;
        qpsk_demod(s, bits);
        rx_bits_raw[config->bps * i] = bits[1];
        rx_bits_raw[config->bps * i + 1] = bits[0];
    }

    Nerrs = 0;

    for (i = 0; i < coded_bits_per_frame; i++) {
        if (test_codeword[i] != rx_bits_raw[i]) {
            Nerrs++;
        }
    }

    *Nerrs_raw = Nerrs;
    Terrs += Nerrs;

    return Terrs;
}

int count_errors(uint8_t tx_bits[], uint8_t rx_bits[], int n) {
    int i;
    int Nerrs = 0;

    for (i = 0; i < n; i++) {
        if (tx_bits[i] != rx_bits[i]) {
            Nerrs++;
        }
    }
    
    return Nerrs;
}

/*
   Given an array of tx_bits, LDPC encodes, interleaves, and OFDM modulates
 */

void ofdm_ldpc_interleave_tx(struct OFDM *ofdm, struct LDPC *ldpc, complex float tx_sams[], uint8_t tx_bits[], uint8_t txt_bits[]) {
    int Npayloadsymsperpacket = ldpc->coded_bits_per_frame/ofdm->bps;
    int Npayloadbitsperpacket = ldpc->coded_bits_per_frame;
    int Nbitsperpacket = ofdm_get_bits_per_packet(ofdm);
    int codeword[Npayloadbitsperpacket];
    COMP payload_symbols[Npayloadsymsperpacket];
    COMP payload_symbols_inter[Npayloadsymsperpacket];
    complex float tx_symbols[Nbitsperpacket/ ofdm->bps];

    ldpc_encode_frame(ldpc, codeword, tx_bits);
    qpsk_modulate_frame(payload_symbols, codeword, Npayloadsymsperpacket);
    gp_interleave_comp(payload_symbols_inter, payload_symbols, Npayloadsymsperpacket);
    ofdm_assemble_qpsk_modem_packet_symbols(ofdm, tx_symbols, payload_symbols_inter, txt_bits);
    ofdm_txframe(ofdm, tx_sams, tx_symbols);
}

