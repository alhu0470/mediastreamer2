/*
 mediastreamer2 library - modular sound and video processing and streaming
 Copyright (C) 2015  Belledonne Communications <info@belledonne-communications.com>
 
 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 as published by the Free Software Foundation; either version 2
 of the License, or (at your option) any later version.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include "h264utils.h"
#include <mediastreamer2/msqueue.h>

typedef struct _MSBitsReader {
	const uint8_t *data;
	uint8_t mask;
	int index;
} MSBitsReader;

static void ms_bits_reader_init(MSBitsReader *br, const uint8_t *data) {
	br->data = data;
	br->mask = 0x80;
	br->index = 0;
}

static unsigned int ms_bits_reader_read_as_uint(MSBitsReader *br, int n) {
	unsigned int value = 0;
	int i;
	for(i=0; i<n; i++) {
		if (i != 0) {
			value <<= 1;
		}
		if (br->data[br->index] & br->mask) {
			value |= 0x1;
		}
		br->mask >>= 1;
		if (br->mask == 0) {
			br->mask = 0x80;
			br->index++;
		}
	}
	return value;
}

static unsigned int ms_h264_exp_golomb_code_to_uint(const uint8_t *data) {
	int leading_zero_bits = -1;
	bool_t b = FALSE;
	MSBitsReader bits_reader;
	
	ms_bits_reader_init(&bits_reader, data);
	while (!b) {
		b = ms_bits_reader_read_as_uint(&bits_reader, 1);
		leading_zero_bits++;
	}
	return (0x1<<leading_zero_bits) - 1 + ms_bits_reader_read_as_uint(&bits_reader, leading_zero_bits);
}

static void  push_nalu(const uint8_t *begin, const uint8_t *end, MSQueue *nalus){
    unsigned ecount = 0;
    const uint8_t *src=begin;
    size_t nalu_len = (end-begin);
    uint8_t nalu_byte  = *src++;
    
    mblk_t *m=allocb(nalu_len,0);
    
    // Removal of the 3 in a 003x sequence
    // This emulation prevention byte is normally part of a NAL unit.
    /* H.264 standard sys in par 7.4.1 page 58
     emulation_prevention_three_byte is a byte equal to 0x03.
     When an emulation_prevention_three_byte is present in a NAL unit, it shall be discarded by the decoding process.
     Within the NAL unit, the following three-byte sequence shall not occur at any byte-aligned position: 0x000000, 0x000001, 0x00002
     */
    *m->b_wptr++=nalu_byte;
    while (src<end-3) {
        if (src[0]==0 && src[1]==0 && src[2]==3){
            *m->b_wptr++=0;
            *m->b_wptr++=0;
            // drop the emulation_prevention_three_byte
            src+=3;
            ++ecount;
            continue;
        }
        *m->b_wptr++=*src++;
    }
    *m->b_wptr++=*src++;
    *m->b_wptr++=*src++;
    *m->b_wptr++=*src++;
    
    ms_queue_put(nalus, m);
}

void ms_h264_bitstream_to_nalus(const uint8_t *bitstream, size_t size, MSQueue *nalus){
    size_t i;
    const uint8_t *p,*begin=NULL;
    int zeroes=0;
    
    for(i=0,p=bitstream;i<size;++i){
        if (*p==0){
            ++zeroes;
        }else if (zeroes>=2 && *p==1 ){
            if (begin){
                push_nalu(begin, p-zeroes, nalus);
            }
            begin=p+1;
        }else zeroes=0;
        ++p;
    }
    if (begin) push_nalu(begin, p, nalus);
}

MSH264NaluType ms_h264_nalu_get_type(const mblk_t *nalu) {
    return (*nalu->b_rptr) & ((1<<5)-1);
}

unsigned int ms_h264_sps_get_id(const mblk_t *sps) {
	return ms_h264_exp_golomb_code_to_uint(sps->b_rptr+4);
}

unsigned int ms_h264_pps_get_id(const mblk_t *pps) {
	return ms_h264_exp_golomb_code_to_uint(pps->b_rptr+1);
}

void ms_h264_stream_to_nalus(const uint8_t *frame, size_t size, MSQueue *nalus, int *idr_count) {
    const uint8_t *ptr = frame;
    
    if(idr_count) *idr_count = 0;
    
    while (ptr < frame + size) {
        uint32_t nalu_size;
        mblk_t *nalu;
        MSH264NaluType nalu_type;
        
        memcpy(&nalu_size, ptr, 4);
        nalu_size = ntohl(nalu_size);
        
        nalu = allocb(nalu_size, 0);
        memcpy(nalu->b_wptr, ptr+4, nalu_size);
        ptr+=nalu_size+4;
        nalu->b_wptr+=nalu_size;
        
        nalu_type = ms_h264_nalu_get_type(nalu);
        if(idr_count && nalu_type == MSH264NaluTypeIDR) (*idr_count)++;
        
        ms_queue_put(nalus, nalu);
    }
}
