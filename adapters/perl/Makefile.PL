use ExtUtils::MakeMaker;

WriteMakefile(
    NAME         => 'SRR',
    VERSION_FROM => 'lib/SRR.pm',
    PREREQ_PM    => {
        'IO::Socket::UNIX' => 0,
        'IO::Socket::INET' => 0,
        'Time::HiRes'      => 0,
    },
    ABSTRACT     => 'Synchronous Send/Receive/Reply client and wire protocol library',
);
