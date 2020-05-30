from parser import handleQuery 
def main(): 
    # Use the same options for every query. TODO: Allow options changes while entering queries. 
    print("Enter command options as command separated values belows. Press 'Enter' without input to use default options.")
    print("Options (must be in this order): [num_objs],[pool_name],[use-cls],[quiet]")
    print("Example: 2, tpchdata, [Y | N], [Y | N]")
    opts = input(">>> ")

    # Run until told otherwise.
    while(1):
        print("Enter a SQL query (Multiple semi-colon separated queries can be accepted).")
        rawUserQuery = input(">>> ")
        handleQuery(opts, rawUserQuery)


if __name__ == "__main__":
    main()
