#! /usr/bin/perl 
use strict;
use warnings;

use TestLibFinder;
use lib test_lib_loc();
 
use CRI::Test;
plan('no_plan'); 
setDesc('make install');

my $build_dir   = test_lib_loc().'/../..';
runCommand("cd $build_dir; make install", test_success_die => 1);

runCommand('ldconfig');