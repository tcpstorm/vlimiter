; present_stub.asm - MSVC (MASM x64) version of the mid-function detour entry
; in src/game/present.cpp. The mingw build uses the inline-asm block there;
; this file is compiled only when CMake sees MSVC. Keep the two in step.
;
; Preserves flags, rax, rcx, rdx, r8-r11, xmm0-xmm5 and rbx; realigns the
; stack for the call; then jumps to MinHook's trampoline.

EXTERN vlimiter_before_present:PROC
EXTERN vlimiter_present_trampoline:QWORD

.code

vlimiter_present_stub PROC
	pushfq
	push rax
	push rcx
	push rdx
	push r8
	push r9
	push r10
	push r11
	push rbx
	mov rbx, rsp
	and rsp, -16
	sub rsp, 80h
	movups xmmword ptr [rsp+20h], xmm0
	movups xmmword ptr [rsp+30h], xmm1
	movups xmmword ptr [rsp+40h], xmm2
	movups xmmword ptr [rsp+50h], xmm3
	movups xmmword ptr [rsp+60h], xmm4
	movups xmmword ptr [rsp+70h], xmm5
	call vlimiter_before_present
	movups xmm0, xmmword ptr [rsp+20h]
	movups xmm1, xmmword ptr [rsp+30h]
	movups xmm2, xmmword ptr [rsp+40h]
	movups xmm3, xmmword ptr [rsp+50h]
	movups xmm4, xmmword ptr [rsp+60h]
	movups xmm5, xmmword ptr [rsp+70h]
	mov rsp, rbx
	pop rbx
	pop r11
	pop r10
	pop r9
	pop r8
	pop rdx
	pop rcx
	pop rax
	popfq
	jmp qword ptr [vlimiter_present_trampoline]
vlimiter_present_stub ENDP

END
