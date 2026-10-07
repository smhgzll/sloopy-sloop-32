# Convenience targets; every target is a script in scripts/.
.PHONY: setup fetch doctor host test-host test-upstream web build-qemu qemu test-qemu config build flash monitor clean test

setup:          ; ./scripts/bootstrap-nobara.sh
fetch:          ; ./scripts/fetch-sloop.sh
doctor:         ; ./scripts/doctor.sh
host:           ; ./scripts/build-host.sh
test-host:      ; ./scripts/test-host.sh
test-upstream:  ; ./scripts/test-upstream.sh
web:            ; ./scripts/serve-web.sh
build-qemu:     ; ./scripts/build-qemu.sh
qemu:           ; ./scripts/run-qemu.sh
test-qemu:      ; ./scripts/test-qemu.sh
config:         ; ./scripts/configure-hardware.sh
build:          ; ./scripts/build-hardware.sh
flash:          ; ./scripts/flash.sh $(PORT)
monitor:        ; ./scripts/monitor.sh $(PORT)
clean:          ; ./scripts/clean.sh
test: test-host test-qemu
