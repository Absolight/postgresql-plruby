
create function plruby_call_handler() returns opaque
    as '/h/nblg/ts/ruby/perso/plruby-0.2.7/plruby.so'
   language 'C';

   create trusted  language 'plruby'
	handler plruby_call_handler
	lancompiler 'PL/Ruby';
