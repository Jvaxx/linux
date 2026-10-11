# SPDX-License-Identifier: GPL-2.0-only
#
# Built from tools/aurora-sep: rpmbuild --define "_sourcedir <that directory>"
# (build.sh does this).

Name:           aurora-sep
Version:        1
Release:        1%{?dist}
Summary:        Loader and fprintd policy for the Apple SEP fingerprint driver

License:        GPL-2.0-only
URL:            https://github.com/aurora-silicon/linux
Source0:        load-driver
Source1:        aurora-sep.service
Source2:        fprintd-aurora.conf
Source3:        README.md
Source4:        XART-SAFETY.md

BuildArch:      noarch
BuildRequires:  systemd-rpm-macros
Requires:       coreutils
Requires:       fprintd
Requires:       keyutils
Requires:       kmod
%{?systemd_requires}

%description
Loads the Apple SEP driver (apple_sep) with xART writes enabled, waits until
the secure enclave seals and unseals a trusted key and /dev/sep-bio appears,
and lets fprintd reach /dev/sep-bio. The loader runs at boot, and fprintd
also pulls it in when it starts. libfprint needs the aurora driver, which
this package does not provide. Read XART-SAFETY.md before installing it on
a machine that also runs macOS.

%prep
%setup -q -c -T
cp -p %{SOURCE3} %{SOURCE4} .

%build

%install
install -Dpm 0755 %{SOURCE0} %{buildroot}%{_libexecdir}/aurora-sep-load
install -Dpm 0644 %{SOURCE1} %{buildroot}%{_unitdir}/aurora-sep.service
sed -i 's|^ExecStart=.*|ExecStart=%{_libexecdir}/aurora-sep-load|' \
    %{buildroot}%{_unitdir}/aurora-sep.service
grep -qx 'ExecStart=%{_libexecdir}/aurora-sep-load' \
    %{buildroot}%{_unitdir}/aurora-sep.service
install -Dpm 0644 %{SOURCE2} \
    %{buildroot}%{_unitdir}/fprintd.service.d/aurora-sep.conf
# Load the driver at boot, so the enclave is up before the login screen asks.
install -d %{buildroot}%{_presetdir}
echo 'enable aurora-sep.service' >%{buildroot}%{_presetdir}/80-aurora-sep.preset

%post
%systemd_post aurora-sep.service

%preun
%systemd_preun aurora-sep.service

%postun
# Never restart it: the driver attaches to the enclave once per boot.
%systemd_postun aurora-sep.service

%files
%doc README.md XART-SAFETY.md
%{_libexecdir}/aurora-sep-load
%{_unitdir}/aurora-sep.service
%dir %{_unitdir}/fprintd.service.d
%{_unitdir}/fprintd.service.d/aurora-sep.conf
%{_presetdir}/80-aurora-sep.preset

%changelog
* Sun Oct 11 2026 Ryan Murray <ryan@aurorasilicon.org> - 1-1
- Package the loader, its service and the fprintd policy
