#!/usr/bin/perl

use strict;
use warnings;

use FindBin;
use TestLibFinder;
use lib test_lib_loc();


use CRI::Test;

use Torque::Util        qw( run_and_check_cmd );
use Torque::Test::Qstat::Utils qw( list_queue_info   );

plan('no_plan');
setDesc("qmgr -c create and delete a queue");

# Variables
my $cmd;
my %qmgr;
my $scheduling;
my %queue_info;

# Create a unique queue name
my $qname = 'test_q';

# Test creating a queue
$cmd  = "qmgr -a -c 'create queue $qname'";
%qmgr = run_and_check_cmd($cmd);

%queue_info = list_queue_info($qname);
ok(exists $queue_info{ $qname }, "Checking that the queue was created");

# Test deleting a queue
$cmd  = "qmgr -a -c 'delete queue $qname'";
%qmgr = run_and_check_cmd($cmd);

%queue_info = list_queue_info();
ok(! exists $queue_info{ $qname }, "Checking that the queue was deleted");
