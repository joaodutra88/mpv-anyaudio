# Makes Nuvio Desktop load its own libmpv again. Restart Nuvio afterwards.
[Environment]::SetEnvironmentVariable('NUVIO_LIBMPV_PATH', $null, 'User')
'anyaudio removed from Nuvio. You can delete this folder.'
