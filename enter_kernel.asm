[bits 32]

;
; Part 4
;
; Code for jumpting to C kernel.
; Implement your routine for entering the kernel here
;

extern kernel_main
global enter_kernel
enter_kernel:
  call kernel_main
  jmp $