; Exercise the emitted budget stub with the same CALL/stack-slot convention as the game.
.code
PUBLIC ProbeBudgetStub
ProbeBudgetStub PROC
    push rbx
    sub rsp, 100h
    mov ebx, edx
    mov qword ptr [rsp+rbx-8], 11111111h
    mov qword ptr [rsp+rbx], -1
    mov qword ptr [rsp+rbx+8], 22222222h
    mov rax, 1122334455667788h
    bt r8d, 0
    pushfq
    pop r10
    call rcx
    pushfq
    pop r11
    cmp r10, r11
    jne failed
    mov r9, 1122334455667788h
    cmp rax, r9
    jne failed
    cmp qword ptr [rsp+rbx-8], 11111111h
    jne failed
    cmp qword ptr [rsp+rbx+8], 22222222h
    jne failed
    mov rax, qword ptr [rsp+rbx]
    jmp finished
failed:
    mov rax, -1
finished:
    add rsp, 100h
    pop rbx
    ret
ProbeBudgetStub ENDP
END
