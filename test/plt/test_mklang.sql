
   create function plruby_call_handler() returns language_handler
   as '/home/ts/ruby/perso/plruby-0.4.6/src/plruby.so'
   language 'C';

   create trusted  language 'plruby'
	handler plruby_call_handler
	lancompiler 'PL/Ruby';
