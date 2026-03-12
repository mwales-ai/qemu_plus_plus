crypto_ss.add(genh)
crypto_ss.add(files(
  'afsplit.cpp',
  'akcipher.cpp',
  'block-luks.cpp',
  'block-qcow.cpp',
  'block.cpp',
  'cipher.cpp',
  'der.cpp',
  'hash.cpp',
  'hmac.cpp',
  'ivgen-essiv.cpp',
  'ivgen-plain.cpp',
  'ivgen-plain64.cpp',
  'ivgen.cpp',
  'pbkdf.cpp',
  'secret_common.c',
  'secret.c',
  'tlscreds.c',
  'tlscredsanon.c',
  'tlscredspsk.c',
  'tlscredsx509.c',
  'tlssession.c',
  'rsakey.cpp',
))

if gnutls.found()
  crypto_ss.add(files(
    'tlscredsbox.cpp',
    'x509-utils.cpp',
  ))
endif

if nettle.found()
  crypto_ss.add(nettle, files('hash-nettle.cpp', 'hmac-nettle.cpp', 'pbkdf-nettle.cpp'))
  if hogweed.found()
    crypto_ss.add(gmp, hogweed)
  endif
elif gcrypt.found()
  crypto_ss.add(gcrypt, files('hash-gcrypt.cpp', 'hmac-gcrypt.cpp', 'pbkdf-gcrypt.cpp'))
elif gnutls.found()
  crypto_ss.add(gnutls, files('hash-gnutls.cpp', 'hmac-gnutls.cpp', 'pbkdf-gnutls.cpp'))
else
  crypto_ss.add(files('hash-glib.cpp', 'hmac-glib.cpp', 'pbkdf-stub.cpp'))
endif

if have_keyring
  crypto_ss.add(files('secret_keyring.c'))
endif
if have_afalg
  crypto_ss.add(if_true: files('afalg.cpp', 'cipher-afalg.c', 'hash-afalg.cpp'))
endif

system_ss.add(when: gnutls, if_true: files('tls-cipher-suites.c'))

util_ss.add(files(
  'aes.cpp',
  'clmul.cpp',
  'init.cpp',
  'sm4.cpp',
))
if gnutls.found()
  util_ss.add(gnutls)
endif

if gcrypt.found()
  util_ss.add(gcrypt, files('random-gcrypt.cpp'))
elif gnutls.found()
  util_ss.add(gnutls, files('random-gnutls.cpp'))
elif get_option('rng_none')
  util_ss.add(files('random-none.cpp'))
else
  util_ss.add(files('random-platform.cpp'))
endif

