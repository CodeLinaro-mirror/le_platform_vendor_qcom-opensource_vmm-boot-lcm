Name: vmm-boot-lcm
Version: 1.0
Release: 2%{?dist}
Summary: Guest Vmm Boot LifeCycle Manager
License: BSD-3-Clause-Clear
Source0: %{name}-%{version}.tar.gz

BuildRequires: cmake glib2-devel vmm-lib-devel systemd-devel systemd-rpm-macros libarchive-devel
Requires: glib2 vmm-lib systemd-libs libarchive

%description
Control gvm boot lifecycle

%prep
# nothing to prep

%setup -qn %{name}-%{version}

%build
%cmake
%cmake_build

%install
%cmake_install
mkdir -p %{buildroot}%{_unitdir}
install -DpZm 0644 vmm-boot-lcm.service %{buildroot}%{_unitdir}

%post
systemctl enable vmm-boot-lcm.service

%preun
%systemd_preun vmm-boot-lcm.service

%postun
%systemd_postun_with_restart vmm-boot-lcm.service

%files
%{_bindir}/vmm-boot-lcm
%{_unitdir}/vmm-boot-lcm.service
