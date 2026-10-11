# SPDX-License-Identifier: GPL-2.0-only
#
# Built from tools/aurora-sep: rpmbuild --define "_sourcedir <that directory>"
# (build.sh does this).

Name:           aurora-sep
Version:        1
Release:        2%{?dist}
Summary:        Loader and desktop integration for the Apple SEP fingerprint driver

License:        GPL-2.0-only
URL:            https://github.com/aurora-silicon/linux
Source0:        load-driver
Source1:        aurora-sep.service
Source2:        fprintd-aurora.conf
Source3:        README.md
Source4:        XART-SAFETY.md
Source5:        aurora-sep-ready
Source6:        aurora-sep-ready.service

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
also pulls it in when it starts. Touch ID is then activated, before fprintd
starts. libfprint needs the aurora driver, which this package does not
provide. Read XART-SAFETY.md before installing it on
a machine that also runs macOS.

%prep
%setup -q -c -T
cp -p %{SOURCE3} %{SOURCE4} .

%build

%install
# A unit and the helper it runs ($1, installed as $3), pointed at the
# packaged helper.
unit() {
    install -Dpm 0755 "$1" %{buildroot}%{_libexecdir}/"$3"
    install -Dpm 0644 "$2" %{buildroot}%{_unitdir}/"${2##*/}"
    sed -i "s|^\(Exec[A-Za-z]*=\)/usr/local/sbin/$3|\1%{_libexecdir}/$3|" \
        %{buildroot}%{_unitdir}/"${2##*/}"
    if grep -q /usr/local %{buildroot}%{_unitdir}/"${2##*/}"; then exit 1; fi
}
unit %{SOURCE0} %{SOURCE1} aurora-sep-load
unit %{SOURCE5} %{SOURCE6} aurora-sep-ready
install -Dpm 0644 %{SOURCE2} \
    %{buildroot}%{_unitdir}/fprintd.service.d/aurora-sep.conf
# Load the driver at boot, so the enclave is up before the login screen asks.
install -d %{buildroot}%{_presetdir}
echo 'enable aurora-sep.service' >%{buildroot}%{_presetdir}/80-aurora-sep.preset

%post
%systemd_post aurora-sep.service aurora-sep-ready.service

%preun
%systemd_preun aurora-sep.service aurora-sep-ready.service

%postun
# Never restart them: the driver attaches to the enclave once per boot.
%systemd_postun aurora-sep.service aurora-sep-ready.service

%files
%doc README.md XART-SAFETY.md
%{_libexecdir}/aurora-sep-load
%{_libexecdir}/aurora-sep-ready
%{_unitdir}/aurora-sep.service
%{_unitdir}/aurora-sep-ready.service
%dir %{_unitdir}/fprintd.service.d
%{_unitdir}/fprintd.service.d/aurora-sep.conf
%{_presetdir}/80-aurora-sep.preset

%changelog
* Sun Oct 11 2026 Ryan Murray <ryan@aurorasilicon.org> - 1-2
- Activate Touch ID at boot, before fprintd starts

* Sun Oct 11 2026 Ryan Murray <ryan@aurorasilicon.org> - 1-1
- Package the loader, its service and the fprintd policy
