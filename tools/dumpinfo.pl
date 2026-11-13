#!/usr/bin/perl
# dumpinfo.pl DUMP [ADDRESS...]: read an NT 4.0 crash dump of PINT (ARM), C:\WINNT\MEMORY.DMP,
# which savedump writes at the start after a STOP screen (crash_dump=1 and total_mem= in
# config.txt), copied off the card; or C:\pagefile.sys before that start.
#
# It prints the STOP code and its parameters, the registers at the bug check, the loaded
# drivers (from PsLoadedModuleList, through the page tables of the dump), and the driver
# and offset of every register and parameter that points into one.
#
#     tools/dumpinfo.pl MEMORY.DMP
#     tools/dumpinfo.pl MEMORY.DMP F5849F12 ...       (also place these addresses)
#
# The dump is a header page (DUMP_HEADER, ntiodump.h: the physical memory runs at ULONG 25,
# the CONTEXT of the bug check at ULONG 200; the ARM CONTEXT is ContextFlags, R0-R12, Sp, Lr,
# Pc, Cpsr), then the pages of the runs in order.
use strict;
use warnings;

my $PAGE = 4096;

@ARGV or die "usage: dumpinfo.pl DUMP [ADDRESS...]\n";
my $path = shift;
open my $f, '<:raw', $path or die "$path: $!\n";
my $header;
read $f, $header, $PAGE;
substr($header, 0, 8) eq 'PAGEDUMP' or die "$path: no dump (it does not start with PAGEDUMP)\n";

sub header { unpack 'V', substr($header, $_[0] * 4, 4) }

my ($dtb, $modules, $processors, $pages) = (header(4), header(6), header(9), header(26));
my @version = (header(2), header(3));
my ($code, @params) = map { header(10 + $_) } 0 .. 4;
my @context = unpack 'V18', substr($header, 200 * 4, 72);

# The runs: first page, pages, offset in the file.
my @runs;
my $offset = $PAGE;
my $count = header(25);
$count = 32 if $count > 32;
for my $i (0 .. $count - 1) {
    push @runs, [header(27 + 2 * $i), header(28 + 2 * $i), $offset];
    $offset += header(28 + 2 * $i) * $PAGE;
}

sub physical {
    my ($address, $length) = @_;
    my $page = int($address / $PAGE);
    my $rest = $PAGE - $address % $PAGE;
    $length = $rest if $length > $rest;
    for my $run (@runs) {
        next unless $run->[0] <= $page && $page < $run->[0] + $run->[1];
        seek $f, $run->[2] + ($page - $run->[0]) * $PAGE + $address % $PAGE, 0;
        my $data;
        read $f, $data, $length;
        return $data;
    }
    return undef;
}

sub physical_ulong {
    my $data = physical($_[0], 4);
    return defined $data && length $data == 4 ? unpack('V', $data) : undef;
}

# PINT's ARM memory manager keeps the MIPS (R4000) design: KSEG0 (0x80000000-0xBFFFFFFF) is
# the first gigabyte of physical memory; the rest goes through NT's own two-level page
# tables (the ARM MMU's are a cache of them), R4000 PTEs (Valid bit 1, the page in bits
# 6-29): the directory, a page whose PTE is the DirectoryTableBase, a PDE for each 4 MB
# (va >> 22), a page of 1024 PTEs under it.
sub translate {
    my ($va) = @_;
    return $va - 0x80000000 if $va >= 0x80000000 && $va < 0xC0000000;
    return undef unless $dtb & 2;
    my $pde = physical_ulong((($dtb >> 6) & 0xFFFFFF) * $PAGE + ($va >> 22) * 4);
    return undef unless defined $pde && $pde & 2;
    my $pte = physical_ulong((($pde >> 6) & 0xFFFFFF) * $PAGE + (($va >> 12) & 0x3FF) * 4);
    return undef unless defined $pte && $pte & 2;
    return (($pte >> 6) & 0xFFFFFF) * $PAGE + ($va & 0xFFF);
}

sub virtual {
    my ($va, $length) = @_;
    my $out = '';
    while ($length > 0) {
        my $pa = translate($va);
        return undef unless defined $pa;
        my $data = physical($pa, $length);
        return undef unless defined $data && length $data;
        $out .= $data;
        $va += length $data;
        $length -= length $data;
    }
    return $out;
}

sub virtual_ulong {
    my $data = virtual($_[0], 4);
    return defined $data ? unpack('V', $data) : undef;
}

# A UNICODE_STRING.
sub unicode {
    my $data = virtual($_[0], 8);
    return '?' unless defined $data;
    my ($length, undef, $buffer) = unpack 'vvV', $data;
    my $text = virtual($buffer, $length > 512 ? 512 : $length);
    return '?' unless defined $text;
    return join '', map { $_ < 256 ? chr $_ : '?' } unpack 'v*', $text;
}

# The loaded drivers (LDR_DATA_TABLE_ENTRY: DllBase at 24, SizeOfImage at 32, BaseDllName at
# 44): base, size, name.
my (@mods, %seen);
my $entry = virtual_ulong($modules);
while ($entry && $entry != $modules && !$seen{$entry}++ && @mods < 256) {
    my $data = virtual($entry, 52);
    last unless defined $data;
    my ($base, undef, $size) = unpack 'V3', substr($data, 24, 12);
    push @mods, [$base, $size, unicode($entry + 44)];
    $entry = unpack 'V', $data;
}

sub place {
    my ($value) = @_;
    for my $mod (@mods) {
        return sprintf '%s+0x%X', $mod->[2], $value - $mod->[0]
            if $mod->[0] <= $value && $value < $mod->[0] + $mod->[1];
    }
    return '';
}

printf "STOP 0x%08X (0x%08X, 0x%08X, 0x%08X, 0x%08X)\n", $code, @params;
my $kind = $version[0] & 0xFFFF;
my $build = $kind == 0xF ? 'free' : $kind == 0xC ? 'checked' : sprintf '0x%X', $version[0];
printf "NT 4.0 build %d (%s), %d processor(s), %d MB in %d run(s)\n",
       $version[1], $build, $processors, ($pages * $PAGE) >> 20, scalar @runs;

my @names = ((map { "R$_" } 0 .. 12), 'SP', 'LR', 'PC', 'CPSR');
print "\nRegisters at the bug check:\n";
printf "  %-4s %08X  %s\n", $names[$_], $context[$_ + 1], place($context[$_ + 1]) for 0 .. $#names;
print "\nParameters:\n";
printf "  P%d   %08X  %s\n", $_ + 1, $params[$_], place($params[$_]) for 0 .. $#params;
for my $argument (@ARGV) {
    my $value = hex $argument;
    printf "  %08X  %s\n", $value, place($value) || '(in no loaded driver)';
}

printf "\nLoaded drivers (%d):\n", scalar @mods;
printf "  %08X-%08X  %s\n", $_->[0], $_->[0] + $_->[1], $_->[2]
    for sort { $a->[0] <=> $b->[0] || $a->[1] <=> $b->[1] || $a->[2] cmp $b->[2] } @mods;
