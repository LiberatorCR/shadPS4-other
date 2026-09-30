Run the configured Windows checkout test suite from the repository root:

```powershell
cmd /c scripts\test_checkout.cmd
```

The command locates Visual Studio's x64 toolchain, builds all four test executables,
and runs every CTest case without filters. Any nonzero build result stops the script,
including negative Windows linker exit codes. Pass another configured test build
directory as its first argument. The default is `Build/review-tests`, configured with
`ENABLE_TESTS=ON`, Clang and Ninja. CMake, CTest, Visual Studio C++ tools and Vulkan
must be available; the GCN tests execute translated shaders on the available GPU.

The suite includes carry/borrow, 64-bit bit comparisons, packed-half constants,
per-component interpolation, scalar descriptor-use dominance, indexed GDS addresses,
packed ancillary multi-use lowering, five-bit bit masks, and alignment shift edges.
The active HTTP host-override case has a per-test JSON fixture, so it runs alongside
the inactive cases instead of being skipped. The fixture does not send network traffic.

On September 30, `all-tests-final-scaffolding.log` in
`Build/got-ir-audit/review-completion-20260930` recorded all 438 cases passing, with
zero failures and zero skipped tests. This verifies that source snapshot; runtime
menu and gameplay acceptance must be checked separately after subsequent changes.
