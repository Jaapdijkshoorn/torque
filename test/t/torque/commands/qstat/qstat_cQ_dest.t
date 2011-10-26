#!/usr/bin/perl

use strict;
use warnings;

use FindBin;
use TestLibFinder;
use lib test_lib_loc();


# Test Modules
use CRI::Test;

use Torque::Test::Regexp       qw(
                                   QSTAT_Q_REGEXP
                                 );
use Torque::Util        qw(
                                   run_and_check_cmd
                                 );
use Torque::Test::Qstat::Utils qw(
                                   parse_qstat_Q
                                 );

# Test Description
plan('no_plan');
setDesc("qstat -Q [destination]");

# Variables
my $cmd;
my %qstat;
my %queue_info;

my $queue1 = $props->get_property( 'torque.queue.one' );
my $queue2 = $props->get_property( 'torque.queue.two' );

my @queues = ($queue1, $queue2);

my @attributes = qw(
                     queue
                     max
                     tot
                     ena
                     str
                     que
                     run
                     hld
                     wat
                     trn
                     ext
                     t
                   );

# Check for the correct output
foreach my $queue (@queues)
  {

  my $msg = "Checking queue '$queue'";
  diag($msg);

  # Run the command
  $cmd   = "qstat -Q $queue";
  %qstat = run_and_check_cmd($cmd);

  # Parse the output
  %queue_info = parse_qstat_Q($qstat{ 'STDOUT' });

  foreach my $attribute (@attributes)
    {

    my $reg_exp = &QSTAT_Q_REGEXP->{ $attribute };
    like($queue_info{ $queue }{ $attribute }, qr/${reg_exp}/, "Checking '$queue' $attribute attribute");

    } # END foreach my $attribue (@attributes)

  } # END foreach my $queue (@queues)