[bits 32]

;
; Part 4
;
; Code for jumpting to C kernel.
; Implement your routine for entering the kernel here
;

extern kernel_main             ; Defined in C; linked with this asm object.
global enter_kernel            ; Visible to the linker / boot code that jumps here.

enter_kernel:
  call kernel_main             ; Run the kernel; normally does not return.
  jmp $                        ; If it returns, idle forever.
