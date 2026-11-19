#!/usr/bin/perl
# GET=... release.pl OWNER/NAME: the newest release of that GitHub repository (no draft or pre-release) that has a
# file for each entry of GET, what an extra takes of it (build/build-extras.sh), and where each comes from, a line
# each:
#   tag TAG
#   file NAME URL SHA256   for NAME=PATTERN: the file of the release whose name matches PATTERN (* and ?), as NAME
#   zip URL SHA256         for PATTERN (a .zip): the file of the release, to unpack
#   repo NAME URL          for NAME=/PATH: the file PATH of the repository at TAG
# SHA256 is the one GitHub has of the file (empty when it has none). Exit 2: GitHub did not answer; 1: no release
# has the files. GitHub answers 60 requests an hour from an address, 5000 with GITHUB_TOKEN.
use strict;
use warnings;
use JSON::PP;

my $repo = shift or die "usage: GET=... release.pl OWNER/NAME\n";
my @get = split ' ', $ENV{GET} // '';
my @h = ('-H', 'Accept: application/vnd.github+json');
push @h, '-H', "Authorization: Bearer $ENV{GITHUB_TOKEN}" if $ENV{GITHUB_TOKEN};
open(my $c, '-|', 'curl', '-fsSL', '--retry', '3', @h, "https://api.github.com/repos/$repo/releases?per_page=30")
    or do { warn "release.pl: curl: $!\n"; exit 2 };
my $json = do { local $/; <$c> };
close $c or do { warn "release.pl: GitHub did not list the releases of $repo\n"; exit 2 };
my $releases = eval { decode_json($json) } or do { warn "release.pl: GitHub's answer for $repo is no list\n"; exit 2 };

sub pattern { my $p = quotemeta shift; $p =~ s/\\\*/.*/g; $p =~ s/\\\?/./g; qr/^$p$/ }

RELEASE: for my $r (@$releases) {
    next if $r->{draft} || $r->{prerelease};
    my @out = ("tag $r->{tag_name}");
    for my $g (@get) {
        my ($name, $what) = $g =~ /^([^=]*)=(.*)$/ ? ($1, $2) : ('', $g);
        if ($what =~ m{^/(.+)}) {
            push @out, "repo $name https://raw.githubusercontent.com/$repo/$r->{tag_name}/$1";
            next;
        }
        my $re = pattern($what);
        my @a = grep { $_->{name} =~ $re } @{$r->{assets}};
        next RELEASE unless @a;
        if (@a > 1) { warn "release.pl: $what matches " . scalar(@a) . " files of $repo $r->{tag_name}\n"; exit 1 }
        (my $sum = $a[0]{digest} // '') =~ s/^sha256://;
        push @out, $name eq '' ? "zip $a[0]{browser_download_url} $sum" : "file $name $a[0]{browser_download_url} $sum";
    }
    print "$_\n" for @out;
    exit 0;
}
warn "release.pl: no release of $repo has " . join(" ", grep { !m{=/} } @get) . "\n";
exit 1;
