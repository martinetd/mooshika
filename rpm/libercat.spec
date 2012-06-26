Summary: The libercat library
Name: libercat
Version: 0.1
Release: 1
Group: Application/Devel
Packager: Dominique Martinet <dominique.martinet.ocre@cea.fr>
License: CeCILL
Source: %{name}-%{version}.tar.gz
BuildRoot: /tmp/%{name}-buildroot
Prefix: %{_prefix}
Requires: librdmacm, libibverbs
BuildRequires: librdmacm-devel, libibverbs-devel, gcc

%package devel
Group: Application/Devel
Summary: Development files for libercat
Requires: librdmacm-devel, libibverbs-devel

%description devel
Development files for libercat


%define _topdir .

%description
This package contains the libercat library

%prep
tar xf ../SOURCES/%{name}-%{version}.tar.gz
#%setup

%build
%configure
make

%install
rm -rf %{buildroot}
%makeinstall

%clean
rm -rf %{buildroot}

%files
%{_libdir}/*.so*
%{_libdir}/*.la
%{_bindir}/*

%files devel
%{_includedir}/*
%{_libdir}/*.a

%changelog


