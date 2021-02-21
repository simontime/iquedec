#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "libgwavi/gwavi.h"
#include "libgwavi/gwavi_private.h"

#define GBA_WIDTH  240
#define GBA_HEIGHT 160
#define GBA_LENGTH (GBA_WIDTH * GBA_HEIGHT)

#define GBA_PALETTE_ENTRIES 256

#define GBA_ROM_LOAD_ADDRESS 0x8000000

#define IQUE_FRAME_TABLE_LOCATION   0xb0bfb8
#define IQUE_PALETTE_TABLE_LOCATION 0xb0d8d0
#define IQUE_AUDIO_LOCATION         0xce2c80
#define IQUE_AUDIO_LENGTH           0xeb6c0

#define IQUE_NUM_FRAMES 1606
#define IQUE_FRAME_RATE 15

#define IQUE_AUDIO_CHANNELS    1
#define IQUE_AUDIO_BITS        8
#define IQUE_AUDIO_SAMPLE_RATE 9000

#define SAFE_LENGTH ((GBA_PALETTE_ENTRIES * sizeof(uint16_t)) + (GBA_LENGTH * 2))
#define SAFE_COMPRESSED_LENGTH (GBA_LENGTH * sizeof(uint32_t) * 4)

#define CONVERT_555_888(n) ((((n) & 0x7c00) << 9) | (((n) & 0x3e0) << 6) | (((n) & 0x1f) << 3))

// reimplementation of GBA's LZ77 decompressor
void decompress(uint8_t *in, uint8_t *out)
{
    uint8_t *end = out + (*(uint32_t *)in >> 8); in += sizeof(uint32_t);
    
    while (out < end)
    {
        for (uint8_t flags = *in++, mask = 0x80; mask; mask >>= 1)
        {
            if (flags & mask) // retrieve from sliding buffer
            {
                uint16_t token    = *(uint16_t *)in; in += sizeof(uint16_t);              
                uint16_t distance = (((token & 0x0f) << 8) | ((token & 0xff00) >> 8)) + 1;
                uint16_t length   =  ((token & 0xf0) >> 4) + 3;
                
                while (length--)
                    *out++ = out[-distance];
            }
            else // raw
            {
                *out++ = *in++;
            }
        }
    }
}

void convert_frame(uint8_t *in_image, uint16_t *in_palette, uint8_t *out)
{
    static uint8_t  image  [GBA_LENGTH];
    static uint32_t palette[GBA_PALETTE_ENTRIES];
    
    // decompress LZ77 image data
    decompress(in_image, image);
    
    // convert RGB555 palette to RGB888
    for (int i = 0; i < GBA_PALETTE_ENTRIES; i++)
        palette[i] = CONVERT_555_888(in_palette[i]);
    
    // unpalettises image + flips vertically for AVI
    for (int y = GBA_HEIGHT - 1; y >= 0;        y--)
    for (int x = 0;              x < GBA_WIDTH; x++)
    {
        uint32_t px = palette[image[(y * GBA_WIDTH) + x]];
        
        *out++ = px >> 16;
        *out++ = px >> 8;
        *out++ = px;
    }
}

int main(int argc, char **argv)
{    
    if (argc != 3)
    {
        printf("Usage: %s rom.gba out.avi\n", argv[0]);
        return 0;
    }
    
    FILE *in;
    
    if ((in = fopen(argv[1], "rb")) == NULL)
    {
        perror("Error opening ROM");
        return 1;
    }
    
    // init gwavi
    struct gwavi_t      *gwavi;
    struct gwavi_audio_t gwaudio  = { IQUE_AUDIO_CHANNELS, IQUE_AUDIO_BITS, IQUE_AUDIO_SAMPLE_RATE };
    const char           fourcc[] = { 0, 0, 0, 0 }; // RGB24

    if ((gwavi = gwavi_open(argv[2], GBA_WIDTH, GBA_HEIGHT, fourcc, IQUE_FRAME_RATE, &gwaudio)) == NULL)
    {
        perror("Error opening AVI");
        return 1;
    }
    
    // read frame + palette offsets
    static uint32_t frames  [IQUE_NUM_FRAMES];
    static uint32_t palettes[IQUE_NUM_FRAMES];
    
    fseek(in, IQUE_FRAME_TABLE_LOCATION, SEEK_SET);
    fread(frames, sizeof(uint32_t), IQUE_NUM_FRAMES, in);
    fread(palettes, sizeof(uint32_t), IQUE_NUM_FRAMES, in);
    
    // decode frames
    for (int i = 0; i < IQUE_NUM_FRAMES; i++)
    {
        char            filename  [16];
        static uint8_t  data      [SAFE_LENGTH];
        static uint16_t palette   [GBA_PALETTE_ENTRIES];
        static uint8_t  frame     [GBA_LENGTH * 3];
        
        printf("\rWriting frame %d/%d...", i + 1, IQUE_NUM_FRAMES);
        
        // read frame
        fseek(in, frames[i] - GBA_ROM_LOAD_ADDRESS, SEEK_SET);
        fread(data, 1, SAFE_LENGTH, in);
        
        // read palette
        fseek(in, palettes[i] - GBA_ROM_LOAD_ADDRESS, SEEK_SET);
        fread(palette, sizeof(uint16_t), GBA_PALETTE_ENTRIES, in);
        
        // convert frame using palette
        convert_frame(data, palette, frame);

        // add converted frame to AVI
        gwavi_add_frame(gwavi, frame, GBA_LENGTH * 3);      
    }
    
    puts("\nWriting audio...");
    
    // read audio
    static uint8_t audio[IQUE_AUDIO_LENGTH];
    
    fseek(in, IQUE_AUDIO_LOCATION, SEEK_SET);
    fread(audio, 1, IQUE_AUDIO_LENGTH, in);
    
    // convert signed to unsigned
    for (int i = 0; i < IQUE_AUDIO_LENGTH; i++)
        audio[i] ^= 0x80;
    
    // add audio data to AVI
    gwavi_add_audio(gwavi, audio, IQUE_AUDIO_LENGTH);    
    
    // clean up
    fclose(in);
    gwavi_close(gwavi);
    
    puts("Done!");
    
    return 0;
}