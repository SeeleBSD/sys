#!/bin/bash

# Variables
IMAGE_NAME="obsd_aarch64.qcow2"
VM_PORT=10022
VM_USER="root"
VM_PASS="seelebsd"
QEMU_EFI_CODE="/opt/homebrew/share/qemu/edk2-aarch64-code.fd"
LOCAL_SOURCE_DIR="./"

# Function to start the VM
start_vm() {
  echo "Starting the QEMU VM..."
  qemu-system-aarch64 \
    -machine virt \
    -cpu host \
    -accel hvf \
    -m 8192 \
    -bios "${QEMU_EFI_CODE}" \
    -drive if=virtio,file="${IMAGE_NAME}",format=qcow2 \
    -netdev user,id=net0,hostfwd=tcp::${VM_PORT}-:22 \
    -device virtio-net,netdev=net0 \
    -nographic &
  VM_PID=$!
  echo "VM started with PID ${VM_PID}"
  echo "Waiting for VM to boot up..."
  sleep 60  # Adjust the sleep time as needed
}

# Function to stop the VM
stop_vm() {
  echo "Stopping the QEMU VM..."
  sshpass -p ${VM_PASS} ssh -p ${VM_PORT} -o StrictHostKeyChecking=no "${VM_USER}@localhost" << EOF
    shutdown -p now
EOF
}

# Function to copy the source files to the VM
copy_sources() {
  echo "Copying kernel source files to the VM..."
  sshpass -p ${VM_PASS} ssh -p ${VM_PORT} -o StrictHostKeyChecking=no "${VM_USER}@localhost" << EOF
    rm -rf /usr/src/sys
    pkg_add rsync--
EOF
  sshpass -p ${VM_PASS} rsync --exclude "${IMAGE_NAME}" --exclude "*git" --exclude "bsd" -av -e "ssh -p ${VM_PORT} -o StrictHostKeyChecking=no" "${LOCAL_SOURCE_DIR}" "${VM_USER}@localhost:/usr/src/sys"
}

# Function to compile the kernel inside the VM
compile_kernel() {
  echo "Compiling the kernel inside the VM..."
  sshpass -p ${VM_PASS} ssh -p ${VM_PORT} -o StrictHostKeyChecking=no "${VM_USER}@localhost" << EOF
    pkg_add git rust rust-src rust-rustfmt llvm%17
    cargo install bindgen-cli
    ln -s /root/.cargo/bin/bindgen /usr/bin/bindgen
    cd /usr/src/sys/arch/\$(machine)/conf
    config CUSTOM.MP
    cd /usr/src/sys/arch/\$(machine)/compile/CUSTOM.MP
    # Compile the kernel
    rm -rf obj/bindings obj/uapi
    make -j8
EOF
}

copy_kernel_back() {
  sshpass -p ${VM_PASS} scp -o StrictHostKeyChecking=no -P "${VM_PORT}" "${VM_USER}@localhost:/usr/src/sys/arch/arm64/compile/CUSTOM.MP/obj/bsd" "${LOCAL_SOURCE_DIR}/bsd"
}

# Main script execution
main() {
  start_vm
  copy_sources
  compile_kernel
  copy_kernel_back
  stop_vm
}

main
