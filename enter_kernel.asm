[bits 32]

;
; Part 4
;
; Code for jumpting to C kernel.
; Implement your routine for entering the kernel here
;

extern kernel_main
global enter_kernel   ; Export the kernel entry symbol expected by the boot code.
enter_kernel:         ; Execution entry point for switching into the C kernel.
  call kernel_main    ; Call the C kernel entry point.
  jmp $               ; If kernel_main returns, halt here forever.