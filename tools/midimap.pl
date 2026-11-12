#!/usr/bin/perl
# The MIDI Mapper of NT 3.5 (midimap.dll, through which Media Player plays MIDI files)
# plays through the setup of MIDIMAP.CFG that the registry names (Midimap, Mapping Name),
# and a setup names the MIDI output of each of its 16 channels by the device's name. NT's
# file has setups for the sound cards of its day, none for the FM synthesizer of the
# Raspberry Pi's sound driver (drivers/bcmsound), and without a setup whose device exists
# the mapper plays nothing. This writes NT's file with one of its setups renamed and
# pointed at another device: the Windows Sound System's, which like the Pi's is an OPL3
# with the General MIDI patches, all 16 channels and no patch maps.
#
#   midimap.pl IN OUT SETUP NEWSETUP DESCRIPTION DEVICE
#   midimap.pl MIDIMAP.CFG out/MIDIMAP.CFG "Sound System" "Raspberry Pi" "Raspberry Pi FM Synthesizer" "Raspberry Pi FM Synthesizer"
#
# The file (SHELL/CONTROL/MIDI/MIDIMAP.C, packed): a header (version, garbage, the
# current setup's index, the offsets of the setup, patch map and key map tables), each
# table a count of entries and of used ones, then entries of a name (16), a description
# (32), an index and the offset of the data. A setup's data is 16 channels of a channel
# number, a device name (32), a patch map's index and flags.
use strict; use warnings;

my ($in, $out, $setup, $name, $desc, $device) = @ARGV;
die "usage: midimap.pl IN OUT SETUP NEWSETUP DESCRIPTION DEVICE\n" unless defined $device;
die "midimap: $name: a setup's name has at most 15 characters\n" if length($name) > 15;
die "midimap: a description or device name has at most 31 characters\n" if length($desc) > 31 || length($device) > 31;

open(my $h, '<:raw', $in) or die "midimap: $in: $!\n";
my $d = do { local $/; <$h> };
close $h;

my ($version, $garbage, $current, $osetup) = unpack('v V v v', $d);
die "midimap: $in: not version 1\n" unless $version == 1;
my ($entries) = unpack('v', substr($d, $osetup, 2));
my $found;
for my $i (0 .. $entries - 1) {
    my $e = $osetup + 4 + 54 * $i;
    my ($n, $x, $index, $data) = unpack('Z16 Z32 v V', substr($d, $e, 54));
    next unless lc($n) eq lc($setup);
    substr($d, $e, 48) = pack('a16 a32', $name, $desc);
    for my $c (0 .. 15) {
        my $o = $data + 40 * $c;
        # (a channel the setup leaves out has no device)
        substr($d, $o + 2, 32) = pack('a32', $device) if unpack('Z32', substr($d, $o + 2, 32)) ne '';
    }
    # the current setup of the file, for a registry without one
    substr($d, 6, 2) = pack('v', $index);
    $found = 1;
}
die "midimap: $in: no setup $setup\n" unless $found;

open($h, '>:raw', $out) or die "midimap: $out: $!\n";
print $h $d;
close $h;
