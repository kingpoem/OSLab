;
; Print function in 32-bit protected mode: 
;    printing the string starting at the 
;    address stored in B register
;
[bits 32]

VIDEO_MEMORY equ 0xb8000
WHITE_ON_BLACK equ 0x0f

print_string_pm:
	pusha
	mov edx, VIDEO_MEMORY
read_char_pm:
	mov al, [ebx]
	mov ah, WHITE_ON_BLACK
	cmp al, 0
	je  read_done_pm
	mov [edx], ax
	add ebx, 1
	add edx, 2
	jmp read_char_pm
read_done_pm:
	popa
	ret


;
; Print function in 16-bit real mode
;    printing the string starting at the 
;    address stored in B register
;

[bits 16]

print_string:
  pusha
  mov ah, 0xe
read_char:
  mov al, [bx]
  cmp al, 0
  je read_done
  int 0x10
  add bx, 1
  jmp read_char
read_done:
  popa
  ret