openssl req -x509 -sha256 -nodes -newkey rsa:2048 -days 365 -keyout testsite.key -out testsite.crt -subj /CN=www.testsite.local
