Name:           aws-c-mqtt
Version:        0.7.8
Release:        7%{?dist}
Summary:        C99 implementation of the MQTT 3.1.1 specification

License:        ASL 2.0
URL:            https://github.com/awslabs/%{name}
Source0:        %{url}/archive/v%{version}/%{name}-%{version}.tar.gz
Patch0:         aws-c-mqtt-reconnect-api.patch
Patch1:         aws-c-mqtt-cmake.patch

BuildRequires:  gcc
BuildRequires:  cmake
BuildRequires:  openssl-devel
BuildRequires:  aws-c-common-devel
BuildRequires:  aws-c-cal-devel
BuildRequires:  aws-c-io-devel
BuildRequires:  aws-c-compression-devel
BuildRequires:  aws-c-http-devel

Requires:       openssl-devel
Requires:       aws-c-common-libs
Requires:       aws-c-cal-libs
Requires:       aws-c-io-libs
Requires:       aws-c-compression-libs
Requires:       aws-c-http-libs
Requires:       %{name}-libs%{?_isa} = %{version}-%{release}

%description
C99 implementation of the MQTT 3.1.1 specification


%package libs
Summary:        C99 implementation of the MQTT 3.1.1 specification

%description libs
C99 implementation of the MQTT 3.1.1 specification


%package devel
Summary:        C99 implementation of the MQTT 3.1.1 specification
Requires:       %{name}-libs%{?_isa} = %{version}-%{release}

%description devel
C99 implementation of the MQTT 3.1.1 specification


%prep
%autosetup -p1


%build
%cmake -DBUILD_SHARED_LIBS=ON
%cmake_build

%install
%cmake_install

%files
%{_bindir}/elastipubsub

%files libs
%license LICENSE
%doc README.md
%{_libdir}/libaws-c-mqtt.so.1.0.0

%files devel
%dir %{_includedir}/aws/mqtt
%dir %{_includedir}/aws/mqtt/private
%{_includedir}/aws/mqtt/*.h
%{_includedir}/aws/mqtt/private/mqtt_client_test_helper.h

%dir %{_libdir}/cmake/aws-c-mqtt
%dir %{_libdir}/cmake/aws-c-mqtt/shared
%{_libdir}/libaws-c-mqtt.so
%{_libdir}/cmake/aws-c-mqtt/aws-c-mqtt-config.cmake
%{_libdir}/cmake/aws-c-mqtt/shared/aws-c-mqtt-targets-noconfig.cmake
%{_libdir}/cmake/aws-c-mqtt/shared/aws-c-mqtt-targets.cmake



%changelog
* Tue Feb 22 2022 David Duncan <davdunc@amazon.com> - 0.7.8-7
- Updated for package review

* Tue Feb 22 2022 Kyle Knapp <kyleknap@amazon.com> - 0.7.8-6
- Include missing devel directories

* Thu Feb 03 2022 Kyle Knapp <kyleknap@amazon.com> - 0.7.8-5
- Move elastipubsub executable to standard package

* Thu Feb 03 2022 Kyle Knapp <kyleknap@amazon.com> - 0.7.8-4
- Update specfile based on review feedback

* Wed Feb 02 2022 David Duncan <davdunc@amazon.com> - 0.7.8-3
- Prepare for package review

* Tue Jan 25 2022 Kyle Knapp <kyleknap@amazon.com> - 0.7.8-2
- Add patch to make missing API accessible when a shared library

* Tue Jan 18 2022 Kyle Knapp <kyleknap@amazon.com> - 0.7.8-1
- Initial package development
