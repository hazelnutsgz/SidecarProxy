# SidecarProxy

This repo contains the optimization demo for the multiple-thread sidecar proxy
- epoll_rpoxy_et_mode.cc (Baseline: Edge Trigger Mode Proxy)
- zerocopy_epoll_proxy.cc (Zero copy optimization for epoll-base sidecar proxy)
- io_uring_proxy.cc (io_uring based sidecar proxy)
- io_uring_proxy_uds.cc (io_uring-based sidecar proxy with unix domain socket)
- kernel_patch.diff (kernel patch based upon Linux 5.10.76)
