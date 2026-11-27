#!/usr/bin/perl
# timezones.pl: the time zones of Windows as they are today, for the registry of PINT (NT 4.0 and 3.5), their
# rules computed from the IANA tz database. NT's own list is of 1994-1996, and most of its rules have changed
# since (the US and Canada in 2007; Russia, Mexico, Brazil, Iran, Turkey ... no longer keep daylight time;
# Samoa crossed the date line; British Columbia, Alberta, the Northwest Territories and Manitoba keep one
# offset all year from 2026), and Windows has nearly three times as many zones.
#
#   timezones.pl [options] WINDOWSZONES.XML TZDIR SOFTWARE.INI > OUT
#
#   WINDOWSZONES.XML  Unicode CLDR's common/supplemental/windowsZones.xml: every time zone of Windows (its key,
#                     its display name in a comment) and the tz zone it is modeled on (territory 001)
#   TZDIR             the tz database compiled by zic -d (zdump reads it), of the release named by -release
#   SOFTWARE.INI      NT's software.ini (a regini script), whose key Time Zones holds the zones being updated
#   -format regini    (default) a regini script of the key Time Zones: for each zone Display, Dlt, MapID
#                     (with -mapid) and TZI
#   -format reg       a .reg file (REGEDIT4) of the same, for NT 4.0's REGEDIT
#   -mapid            write each zone's MapID (the highlight of the Date/Time world map): NT 4.0 has it, 3.5 not
#   -block N          the zones alone, their keys indented N spaces and their values N+4: the key Time Zones of
#                     a software.ini
#   -release NAME     the tz release, for the comment the output starts with
#   -report FILE      a line for each zone: its tz zone, its rule, and the years the rule misses
#
# The key Time Zones of ported/oak/software.ini.ntdiff (tz: a release of tzdata compiled by zic), from the NT
# tree's software.ini; then ntdiff.pl update writes the edit script again:
#   NT 4.0: timezones.pl -mapid -block 20 -release 2026f windowsZones.xml tz public/oak/bin/software.ini
#   NT 3.5: timezones.pl -block 20 -release 2026f windowsZones.xml tz PUBLIC/OAK/BIN/SOFTWARE.INI
# To update a system already installed:
#   NT 4.0: timezones.pl -format reg -mapid ... public/oak/bin/software.ini > timezones.reg   (REGEDIT imports it)
#   NT 3.5: timezones.pl ... PUBLIC/OAK/BIN/SOFTWARE.INI > timezone.ini            (\SDKTOOLS\REGINI timezone.ini)
#
# What is kept of NT's list: its key names (a key Windows has renamed takes its successor's rule and display
# name: %rename below), its daylight names (Dlt) and of NT 4.0 its MapID. A zone Windows added gets Windows's
# key, a Dlt in Windows's manner (the key with "Daylight" for "Standard"), and MapID -1,-1 (no highlight).
# Every display name is CLDR's (or Microsoft's, %display below) after NT's "(GMT+hh:mm)", the standard offset
# of the zone's rule: CLDR's own "(UTC+hh:mm)" can be older than tz.
#
# The rule of a zone is that of the years 2028 to 2047 (after the 2026 and 2027 exceptions of Canada and
# Cuba): its standard offset and, if it keeps daylight time, the two transitions as NT describes them, the
# n-th (or last) weekday of a month at a local time, each in the offset it ends. NT has one rule a zone for
# every year (the "dynamic" rules came with Vista), so a rule tz states as "the Friday before the last Sunday"
# or "the first Sunday on or after the 2nd" is the form that is right in most of those years; the years it
# misses are in the report.
use strict; use warnings;
use POSIX qw(floor);

my %o = (format => 'regini', release => '?', report => '', mapid => 0);
while (@ARGV && $ARGV[0] =~ /^-(\w+)$/) { shift; my $k = $1; $o{$k} = $k eq 'mapid' ? 1 : shift }
my ($cldr, $tzdir, $legacy) = @ARGV;
die "usage: timezones.pl [-format regini|reg] [-mapid] [-block N] [-release NAME] [-report FILE] WINDOWSZONES.XML TZDIR SOFTWARE.INI\n"
    unless defined $legacy && $o{format} =~ /^(regini|reg)$/;

# keys of NT 4.0 and 3.5 that Windows renamed since: the zone they are now (by the cities each named and its
# offset)
my %rename = ('Mexico Standard Time' => 'Central Standard Time (Mexico)',
              'Mexico Standard Time 2' => 'Mountain Standard Time (Mexico)',
              'Mid-Atlantic Standard Time' => 'UTC-02',
              'GMT' => 'GMT Standard Time', 'GMT Standard Time' => 'Greenwich Standard Time',
              'Lisbon Warsaw Standard Time' => 'Central European Standard Time',
              'Czech Standard Time' => 'Central Europe Standard Time', 'GFT Standard Time' => 'GTB Standard Time',
              'Sydney Standard Time' => 'AUS Eastern Standard Time', 'Bangkok Standard Time' => 'SE Asia Standard Time',
              'Saudi Arabia Standard Time' => 'Arab Standard Time');

# display names Microsoft gives the 2026 zones of Canada, which CLDR shortens
my %display = ('British Columbia Standard Time' => 'Pacific Time (British Columbia)',
               'Alberta Standard Time' => 'Mountain Time (Alberta)',
               'Manitoba Standard Time' => 'Manitoba',
# and one CLDR has not followed: Chihuahua moved to UTC-6 in 2022 (Central Standard Time (Mexico))
               'Mountain Standard Time (Mexico)' => 'La Paz, Mazatlan');

# --- the zones of Windows (CLDR)
my (@win, %tz, %disp);
{
    open my $f, '<', $cldr or die "$cldr: $!\n"; my $comment;
    while (<$f>) {
        $comment = $1 if /<!--\s*\(UTC[^)]*\)\s*(.*?)\s*-->/;
        if (/<mapZone other="([^"]+)" territory="001" type="([^"]+)"/) {
            push @win, $1; $tz{$1} = $2; $disp{$1} = $display{$1} // $comment // $1; $comment = undef;
        }
    }
}
die "no zones in $cldr\n" unless @win;

# --- NT's zones: the key Time Zones of a regini script, its block ending at the first line no deeper than its
# own
my (@leg, %leg);
{
    open my $f, '<', $legacy or die "$legacy: $!\n"; local $/; my $t = <$f>; $t =~ s/\r//g;
    my ($cur, $in); my $w = sub { my $s = shift; $s =~ s/\t/        /g; length $s };
    for (split /\n/, $t) {
        if (!defined $in) { $in = $w->($1) if /^(\s*)Time Zones\b/; next }
        next unless /\S/;
        /^(\s*)/; last if $w->($1) <= $in;
        if (/^\s+(\S.*?)\s*$/ && !/=/) { $cur = $1; push @leg, $cur; $leg{$cur} = {} }
        elsif ($cur && /^\s+(\w+)\s*=\s*(.*?)\s*$/) { $leg{$cur}{$1} = $2 }
    }
}
die "no key Time Zones in $legacy\n" unless @leg;

# --- the rule of a tz zone
sub days_in { my ($y, $m) = @_; return (31, ($y % 4 == 0 && ($y % 100 != 0 || $y % 400 == 0)) ? 29 : 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31)[$m - 1] }
sub dow { my ($y, $m, $d) = @_; my @t = (0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4); $y -= 1 if $m < 3; ($y + int($y / 4) - int($y / 100) + int($y / 400) + $t[$m - 1] + $d) % 7 }
# the day of the n-th (5: last) weekday w of month m
sub nth { my ($y, $m, $w, $n) = @_; my $first = 1 + ($w - dow($y, $m, 1) + 7) % 7; my $d = $first + 7 * ($n - 1);
    $d -= 7 while $d > days_in($y, $m); $d }
my %mon = (Jan => 1, Feb => 2, Mar => 3, Apr => 4, May => 5, Jun => 6, Jul => 7, Aug => 8, Sep => 9, Oct => 10, Nov => 11, Dec => 12);
my (%rule, @report);
sub rule {
    my $zone = shift; return $rule{$zone} if $rule{$zone};
    my @tr; my $prev;
    # (":" before the file's path: glibc's zdump takes a bare path for a zone name and finds no transitions)
    open my $z, '-|', 'zdump', '-v', '-c', '2028,2048', ":$tzdir/$zone" or die "zdump: $!\n";
    while (<$z>) {
        next unless /UT = \w+ (\w+)\s+(\d+) (\d+):(\d+):(\d+) (\d+) (\S+) isdst=(\d) gmtoff=(-?\d+)/;
        my $l = { mon => $mon{$1}, day => $2, h => $3, m => $4, s => $5, y => $6, dst => $8, off => $9 };
        if ($prev && $prev->{off} != $l->{off}) { push @tr, [$prev, $l] }
        $prev = $l;
    }
    close $z;
    my $r = { std => undef, dst => undef, start => undef, end => undef, miss => [] };
    if (!@tr) {
        local $ENV{TZ} = ":$tzdir/$zone"; POSIX::tzset();
        my $z2 = POSIX::strftime('%z', localtime(1893456000));          # 2030-01-01
        $z2 =~ /([+-])(\d\d)(\d\d)/; $r->{std} = ($1 eq '-' ? -1 : 1) * ($2 * 3600 + $3 * 60);
        POSIX::tzset();
        return $rule{$zone} = $r;
    }
    my (%starts, %ends);
    for (@tr) {
        my ($b, $a) = @$_;
        # the local time of the transition in the offset it ends: the last second before it, plus one
        my ($y, $m, $d, $h, $mi, $s) = ($b->{y}, $b->{mon}, $b->{day}, $b->{h}, $b->{m}, $b->{s} + 1);
        if ($s == 60) { $s = 0; $mi += 1 } if ($mi == 60) { $mi = 0; $h += 1 }
        if ($h == 24) { $h = 0; $d += 1; if ($d > days_in($y, $m)) { $d = 1; $m += 1 } }
        my $ev = { y => $y, m => $m, d => $d, h => $h, mi => $mi, s => $s };
        if ($a->{dst} && !$b->{dst}) { $r->{std} = $b->{off}; $r->{dst} = $a->{off}; push @{$starts{$y}}, $ev }
        elsif ($b->{dst} && !$a->{dst}) { $r->{std} = $a->{off}; $r->{dst} = $b->{off}; push @{$ends{$y}}, $ev }
        else { push @{$r->{miss}}, "offset change $b->{off} -> $a->{off} in $y" }
    }
    for my $which (['start', \%starts], ['end', \%ends]) {
        my ($name, $ev) = @$which; my @years = sort keys %$ev;
        next unless @years;
        # the candidate forms: weekday w of week n (1-4, 5 last) of month m at a time; a transition at
        # 00:00 also as 23:59:59.999 of the day before (24:00 of a weekday, as Windows writes it)
        my %votes;
        for my $y (@years) { for my $e (@{$ev->{$y}}) {
            my @c = ([$e->{m}, $e->{d}, $e->{h}, $e->{mi}, $e->{s}, 0]);
            if ($e->{h} == 0 && $e->{mi} == 0 && $e->{s} == 0) {
                my ($m, $d) = ($e->{m}, $e->{d} - 1); if ($d == 0) { $m -= 1; $d = days_in($y, $m) }
                push @c, [$m, $d, 23, 59, 59, 999];
            }
            for my $c (@c) {
                my ($m, $d, @t) = @$c; my $w = dow($y, $m, $d); my $n = int(($d - 1) / 7) + 1;
                $votes{join ',', $m, $w, $n, @t} += 1;
                $votes{join ',', $m, $w, 5, @t} += 1 if $d + 7 > days_in($y, $m) && $n != 5;
            }
        } }
        my ($best) = sort { $votes{$b} <=> $votes{$a} || $a cmp $b } keys %votes;
        my ($m, $w, $n, @t) = split /,/, $best;
        my @miss = grep { my $y = $_; !grep { my $e = $_; my $dd = nth($y, $m, $w, $n);
                     my ($em, $ed) = ($e->{m}, $e->{d});
                     ($t[0] == 23 && $t[3] == 999) ? do { my ($pm, $pd) = ($em, $ed - 1); if ($pd == 0) { $pm -= 1; $pd = days_in($y, $pm) } $pm == $m && $pd == $dd }
                                                   : ($em == $m && $ed == $dd) } @{$ev->{$y}} } @years;
        $r->{$name} = [$m, $w, $n, @t];
        push @{$r->{miss}}, "$name misses " . join(' ', @miss) if @miss;
    }
    return $rule{$zone} = $r;
}

# the TZI (REG_TZI_FORMAT): Bias, StandardBias, DaylightBias, StandardDate, DaylightDate
sub tzi {
    my $r = shift;
    my $bias = -$r->{std} / 60;
    my $dbias = defined $r->{dst} ? -($r->{dst} - $r->{std}) / 60 : -60;
    my $st = sub { my $e = shift; return (0) x 8 unless $e; my ($m, $w, $n, $h, $mi, $s, $ms) = @$e; (0, $m, $w, $n, $h, $mi, $s, $ms) };
    my @std = defined $r->{dst} ? $st->($r->{end}) : (0) x 8;
    my @dlt = defined $r->{dst} ? $st->($r->{start}) : (0) x 8;
    return pack('l<3', $bias, 0, $dbias) . pack('v8', @std) . pack('v8', @dlt);
}

# NT's "(GMT+hh:mm)" of a standard offset (seconds east of UTC)
sub gmt { my $s = shift; return '(GMT)' unless $s; my $m = abs($s) / 60;
    sprintf '(GMT%s%02d:%02d)', $s < 0 ? '-' : '+', int($m / 60), $m % 60 }

# --- the list: NT's keys first, then the zones Windows added
my (%want, @list, %used);
$want{$_} = 1 for @win;
for my $k (@leg) {
    my $w = $rename{$k} // $k;
    if (!$want{$w}) { warn "timezones: $k: no zone of Windows today; left out\n"; next }
    push @list, [$k, $w]; $used{$w} = 1;
}
for my $w (@win) { push @list, [$w, $w] unless $used{$w} }

my $regkey = 'SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones';
my @out;
push @out, "; The time zones of Windows today (Unicode CLDR windowsZones.xml), their rules of tz $o{release}",
           "; for the years 2028 to 2047 (tools/timezones.pl)" unless $o{format} eq 'reg' || defined $o{block};
push @out, "REGEDIT4", "" if $o{format} eq 'reg';
push @out, "\\registry\\machine\\software\\microsoft\\windows nt\\currentversion", "    Time Zones" if $o{format} eq 'regini' && !defined $o{block};
my $ki = ' ' x ($o{block} // 8); my $vi = $ki . '    ';
for (@list) {
    my ($k, $w) = @$_;
    my $r = rule($tz{$w}); my $t = tzi($r);
    my $l = $leg{$k} // {};
    my $disp = gmt($r->{std}) . " $disp{$w}";
    (my $dlt = $leg{$k} ? $k : $w) =~ s/Standard Time/Daylight Time/;
    $dlt = $l->{Dlt} if $l->{Dlt};
    my $mapid = $l->{MapID} // '-1,-1';
    push @report, sprintf "%-34s %-32s bias %5d%s%s", $k, $tz{$w}, -$r->{std} / 60,
        defined $r->{dst} ? sprintf(" dst %d start %s end %s", ($r->{dst} - $r->{std}) / 60, join('/', @{$r->{start} // []}), join('/', @{$r->{end} // []})) : ' no dst',
        @{$r->{miss}} ? '  [' . join('; ', @{$r->{miss}}) . ']' : '';
    if ($o{format} eq 'reg') {
        push @out, "[HKEY_LOCAL_MACHINE\\$regkey\\$k]", "\"Display\"=\"$disp\"", "\"Dlt\"=\"$dlt\"",
                   ($o{mapid} ? ("\"MapID\"=\"$mapid\"") : ()), "\"TZI\"=hex:" . join(',', map { sprintf '%02x', $_ } unpack 'C*', $t), "";
    } else {
        push @out, "$ki$k", "${vi}Display = $disp", "${vi}Dlt = $dlt";
        push @out, "${vi}MapID = $mapid" if $o{mapid};
        push @out, "", "${vi}TZI = REG_BINARY 0x2C " . join(' ', map { sprintf '0x%08X', $_ } unpack 'V11', $t), "";
    }
}
print map { "$_\r\n" } @out;
if ($o{report}) { open my $f, '>', $o{report} or die "$o{report}: $!\n"; print $f "$_\n" for @report }
