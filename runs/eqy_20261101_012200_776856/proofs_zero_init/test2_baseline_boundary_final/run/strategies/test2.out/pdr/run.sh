STATUS=ERROR
sby -f test2.out.sby > /dev/null && STATUS=$(awk '{print $1}' test2.out/status)
echo $STATUS > status
case $STATUS in
    PASS)
        echo "Proved equivalence of partition 'test2.out' using strategy 'pdr'"
    ;;
    FAIL)
        echo "Could not prove equivalence of partition 'test2.out' using strategy 'pdr': partitions not equivalent"
    ;;
    UNKNOWN)
        echo "Could not prove equivalence of partition 'test2.out' using strategy 'pdr': equivalence unknown"
    ;;
    TIMEOUT)
        echo "Could not prove equivalence of partition 'test2.out' using strategy 'pdr': timeout"
    ;;
    *)
        cat test2.out/ERROR 2> /dev/null
        echo "Execution of strategy 'pdr' on partition 'test2.out' encountered an error."
        echo "More details can be found in '/tmp/pmux_nopost_sc3wldoo/proofs_zero_init/test2_baseline_boundary_final/run/strategies/test2.out/pdr/test2.out/logfile.txt'."
        exit 1
    ;;
esac
exit 0

