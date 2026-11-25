/*
 * etcpmon_main.c - ETCP Monitor Main Entry Point
 */

#include "etcpmon_gui.h"
#include <stdio.h>

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, 
                   LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;
    
    struct etcpmon_app app;
    
    /* Initialize GUI */
    if (etcpmon_gui_init(&app, hInstance) != 0) {
        MessageBoxA(NULL, "Failed to initialize application", "Error", MB_OK | MB_ICONERROR);
        return 1;
    }
    
    /* Create main window */
    if (!etcpmon_gui_create_window(&app)) {
        MessageBoxA(NULL, "Failed to create main window", "Error", MB_OK | MB_ICONERROR);
        etcpmon_gui_cleanup(&app);
        return 1;
    }
    
    /* Run message loop */
    int result = etcpmon_gui_run(&app);
    
    /* Cleanup */
    etcpmon_gui_cleanup(&app);
    
    return result;
}
