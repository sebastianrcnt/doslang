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
numbuf      db  24 dup(0)
written     dd  0
allocs      dd  0
frees       dd  0

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

; ---------------------------------------------------------------- primitives
; The standard library is written in Ferro; these are the few things it cannot
; say for itself. All cdecl.

extern _GetProcessHeap@0 : near
extern _HeapAlloc@12 : near
extern _HeapFree@12 : near

; fe_rt_write(handle, ptr, len) -> bytes written
public fe_rt_write
fe_rt_write proc near
        push    ebp
        mov     ebp, esp
        push    ebx
        mov     eax, [ebp+8]            ; 1 = stdout, 2 = stderr
        cmp     eax, 2
        je      pick_err
        push    -11
        jmp     pick_done
pick_err:
        push    -12
pick_done:
        call    _GetStdHandle@4
        push    0
        push    offset written
        push    dword ptr [ebp+16]
        push    dword ptr [ebp+12]
        push    eax
        call    _WriteFile@20
        mov     eax, [written]
        pop     ebx
        mov     esp, ebp
        pop     ebp
        ret
fe_rt_write endp

; fe_rt_alloc(n) -> pointer, or zero
public fe_rt_alloc
fe_rt_alloc proc near
        push    ebp
        mov     ebp, esp
        call    _GetProcessHeap@0
        push    dword ptr [ebp+8]
        push    8                       ; HEAP_ZERO_MEMORY
        push    eax
        call    _HeapAlloc@12
        inc     dword ptr [allocs]
        mov     esp, ebp
        pop     ebp
        ret
fe_rt_alloc endp

; fe_rt_free(p)
public fe_rt_free
fe_rt_free proc near
        push    ebp
        mov     ebp, esp
        mov     eax, [ebp+8]
        test    eax, eax
        je      free_done
        call    _GetProcessHeap@0
        push    dword ptr [ebp+8]
        push    0
        push    eax
        call    _HeapFree@12
        inc     dword ptr [frees]
free_done:
        mov     esp, ebp
        pop     ebp
        ret
fe_rt_free endp

; fe_rt_allocs() / fe_rt_frees() -- what the allocator has been asked to do,
; so that a test can insist every allocation was released.
public fe_rt_allocs
fe_rt_allocs proc near
        mov     eax, [allocs]
        ret
fe_rt_allocs endp

public fe_rt_frees
fe_rt_frees proc near
        mov     eax, [frees]
        ret
fe_rt_frees endp

; fe_rt_write_int(handle, value, is_unsigned) -- decimal, with a sign when
; the value is negative and signed was asked for.
public fe_rt_write_int
fe_rt_write_int proc near
        push    ebp
        mov     ebp, esp
        push    ebx
        push    esi
        push    edi
        mov     edi, offset numbuf + 15
        mov     byte ptr [edi], 0
        mov     eax, [ebp+12]
        xor     ebx, ebx                ; ebx = 1 when a '-' is needed
        cmp     dword ptr [ebp+16], 0
        jne     int_digits
        test    eax, eax
        jge     int_digits
        neg     eax
        mov     ebx, 1
int_digits:
        mov     ecx, 10
int_loop:
        xor     edx, edx
        div     ecx
        add     dl, '0'
        dec     edi
        mov     [edi], dl
        test    eax, eax
        jnz     int_loop
        test    ebx, ebx
        je      int_write
        dec     edi
        mov     byte ptr [edi], '-'
int_write:
        mov     esi, offset numbuf + 15
        sub     esi, edi
        push    esi
        push    edi
        push    dword ptr [ebp+8]
        call    fe_rt_write
        add     esp, 12
        pop     edi
        pop     esi
        pop     ebx
        mov     esp, ebp
        pop     ebp
        ret
fe_rt_write_int endp

; fe_rt_write_hex(handle, value)
public fe_rt_write_hex
fe_rt_write_hex proc near
        push    ebp
        mov     ebp, esp
        push    ebx
        push    esi
        push    edi
        mov     edi, offset numbuf + 15
        mov     byte ptr [edi], 0
        mov     eax, [ebp+12]
hex_loop:
        mov     edx, eax
        and     edx, 15
        cmp     dl, 10
        jb      hex_digit
        add     dl, 'a' - 10 - '0'
hex_digit:
        add     dl, '0'
        dec     edi
        mov     [edi], dl
        shr     eax, 4
        test    eax, eax
        jnz     hex_loop
        mov     esi, offset numbuf + 15
        sub     esi, edi
        push    esi
        push    edi
        push    dword ptr [ebp+8]
        call    fe_rt_write
        add     esp, 12
        pop     edi
        pop     esi
        pop     ebx
        mov     esp, ebp
        pop     ebp
        ret
fe_rt_write_hex endp

; fe_rt_exit(code) -- never returns
public fe_rt_exit
fe_rt_exit proc near
        push    ebp
        mov     ebp, esp
        push    dword ptr [ebp+8]
        call    _ExitProcess@4
fe_rt_exit endp

public fe_start_
fe_start_ proc near
        call    fe_main_
        push    eax
        call    _ExitProcess@4
fe_start_ endp

_TEXT ends

end fe_start_
