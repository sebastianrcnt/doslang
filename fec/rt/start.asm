; Ferro runtime: process entry and the trap handler.
;
; The entry point calls the program's `main` and hands its result to
; ExitProcess, so a Ferro program is an ordinary console executable.
; `fe_trap` prints where the program stopped and why, then exits 3.

.386
.model flat

extern _ExitProcess@4 : near
extern _GetStdHandle@4 : near
extern _WriteFile@20 : near
extern fe_main_ : near

_DATA segment dword public 'DATA'

reasons     dd  offset r_bounds, offset r_overflow, offset r_divide
            dd  offset r_unreach, offset r_explicit
r_bounds    db  'index out of bounds',0
r_overflow  db  'integer overflow',0
r_divide    db  'divide by zero',0
r_unreach   db  'reached unreachable code',0
r_explicit  db  'trap',0
r_unknown   db  'trap',0
prefix      db  'ferro: ',0
at_word     db  ' at ',0
colon       db  ':',0
newline     db  13,10,0
numbuf      db  16 dup(0)
written     dd  0

_DATA ends

_TEXT segment dword public 'CODE'

; write_cstr(esi = pointer to a NUL-terminated string) -> void
write_cstr proc near
        push    ebp
        mov     ebp, esp
        push    ebx
        push    esi
        push    edi
        mov     edi, esi
        xor     ecx, ecx
count_loop:
        cmp     byte ptr [edi], 0
        je      count_done
        inc     edi
        inc     ecx
        jmp     count_loop
count_done:
        test    ecx, ecx
        je      write_done
        push    -11                     ; STD_ERROR_HANDLE
        call    _GetStdHandle@4
        push    0                       ; lpOverlapped
        push    offset written
        push    ecx
        push    esi
        push    eax
        call    _WriteFile@20
write_done:
        pop     edi
        pop     esi
        pop     ebx
        mov     esp, ebp
        pop     ebp
        ret
write_cstr endp

; write_uint(eax = value) -> void
write_uint proc near
        push    ebp
        mov     ebp, esp
        push    ebx
        mov     edi, offset numbuf + 15
        mov     byte ptr [edi], 0
        mov     ebx, 10
digit_loop:
        xor     edx, edx
        div     ebx
        add     dl, '0'
        dec     edi
        mov     [edi], dl
        test    eax, eax
        jnz     digit_loop
        mov     esi, edi
        call    write_cstr
        pop     ebx
        mov     esp, ebp
        pop     ebp
        ret
write_uint endp

; fe_trap(reason, file, line) -- cdecl, never returns
public fe_trap
fe_trap proc near
        push    ebp
        mov     ebp, esp
        mov     esi, offset prefix
        call    write_cstr
        mov     eax, [ebp+8]            ; reason
        cmp     eax, 5
        jb      reason_ok
        mov     esi, offset r_unknown
        jmp     reason_write
reason_ok:
        mov     esi, [reasons + eax*4]
reason_write:
        call    write_cstr
        mov     esi, offset at_word
        call    write_cstr
        mov     esi, [ebp+12]           ; file
        call    write_cstr
        mov     esi, offset colon
        call    write_cstr
        mov     eax, [ebp+16]           ; line
        call    write_uint
        mov     esi, offset newline
        call    write_cstr
        push    3
        call    _ExitProcess@4
fe_trap endp

public fe_start_
fe_start_ proc near
        call    fe_main_
        push    eax
        call    _ExitProcess@4
fe_start_ endp

_TEXT ends

end fe_start_
