PL/ruby
=======

  PL/Ruby is a loadable procedural language for the PostgreSQL database
  system that enables the Ruby language to create functions and trigger
  procedures.


Prerequisite
------------

> * ruby 1.8.7 or later (maybe 1.8.6 too)
> * postgresql >= 7.3

  All PostgreSQL headers need to be installed. Command (see `INSTALL` in the
  directory postgresql-7.x.y)

        make install-all-headers 

Installation
------------

        ruby extconf.rb
        make
        make install

  You may need to specify :

        --with-pg-config=<location of the pg_config command of PostgreSQL>

        --disable-conversion
  by default plruby try to convert a postgres type to a ruby class
  This option give the possibility to disable all conversions

       --with-suffix=<suffix to add>

  For example

        ruby extconf.rb --with-suffix=_geo

  will create `plruby_geo.so`

        --with-greenplum
  To build plruby for Greenplum istead of PostgreSQL 


  *Example usage*

        ruby extconf.rb --with-pg-config=/usr/local/bin/pg_config

Test (and examples)
-------------------

  WARNING : if plruby was compiled without --disable-conversion
  you must **FIRST** run `make install` before `make test`

        make test

  this will run the 2 commands :

        ( cd test/plt; ./runtest )
        ( cd test/plp; ./runtest )

  The database `plruby_test` is created and then destroyed. Don't use it if 
  such a database exist on your system.

  Now create the PL/Ruby language in PostgreSQL

  Since the `pg_language` system catalog is private to each database,
  the new language can be created only for individual databases,
  or in the template1 database. In the latter case, it is
  automatically available in all newly created databases.

  The commands to create the new language are:

        create function plruby_call_handler () returns language_handler
        as 'path-to-plruby-shared-lib'
        language 'C';

        create trusted language 'plruby'
        handler plruby_call_handler
        lancompiler 'PL/Ruby';


  The `trusted` keyword on `create language` tells PostgreSQL,
  that all users (not only those with superuser privilege) are
  permitted to create functions with `LANGUAGE 'plruby'`. This is
  absolutely safe, because there is nothing a normal user can do
  with PL/Ruby, to get around access restrictions he/she has.

Documentation
-------------

  see `plruby.rd` and `plruby.html`

Development
-----------

  New releases and sources can be obtained from <http://github.com/knu/postgresql-plruby>

Copying
-------

  This extension module is copyrighted free software by Guy Decoux.

  You can redistribute it and/or modify it under the same term as Ruby.

* * *

Guy Decoux <ts@moulon.inra.fr> (original author, deceased in July 2008)   
Akinori MUSHA <knu@idaemons.org> (current maintainer)
