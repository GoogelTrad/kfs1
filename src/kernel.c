#include "kernel.h"

void kernel_main(void) 
{
    /* Point our pointer directly to the video memory buffer */
    uint16_t* const vga_buffer = (uint16_t*) VGA_ADDRESS;

    /* Build a background character: space ' ' with Light Grey on Black background */
    uint8_t default_color = VGA_COLOR_LIGHT_GREY | (VGA_COLOR_BLACK << 4);
    uint16_t clear_entry = ' ' | (default_color << 8);

    /* Loop through all 2000 cells (80 * 25) to clear the monitor screen cleanly */
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        vga_buffer[i] = clear_entry;
    }

    /* Build our high-contrast "42" display attributes */
    /* Light Green text on Black background */
    uint8_t green_color = VGA_COLOR_LIGHT_GREEN | (VGA_COLOR_BLACK << 4);
    
    /* Write '4' and '2' directly to the first two slots of screen memory */
    vga_buffer[0] = '4' | (green_color << 8);
    vga_buffer[1] = '2' | (green_color << 8);

    /* Safely anchor the CPU so it does not stray into bad instructions */
    while (1) {
        // Safe idle loop
    }
}