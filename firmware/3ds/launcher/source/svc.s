.arm
.align 4

.global svcControlService
.type svcControlService, %function
svcControlService:
    svc 0xB0
    bx lr
